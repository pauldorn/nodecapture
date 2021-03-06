#include <node.h>
#include <node_buffer.h>
#include <nan.h>
#include <pcap/pcap.h>
#include <stdlib.h>
#include <string.h>
#include <queue>
#include <\Program Files\libuv\include\uv.h>

#ifdef _MSC_VER
# include "windows_polyfills.h"
#else
# include <arpa/inet.h>
# include <sys/ioctl.h>
#endif

#if __linux__
# include <dlfcn.h>
  // Without immediate mode some architectures (e.g. Linux with TPACKET_V3)
  // will buffer replies and potentially cause a *long* delay in packet
  // reception

  // pcap_set_immediate_mode is new as of libpcap 1.5.1, so we check for
  // this new method dynamically ...
  typedef void* (*set_immediate_fn)(pcap_t *p, int immediate);
  void* _pcap_lib_handle = dlopen("libpcap.so", RTLD_LAZY);
  set_immediate_fn set_immediate_mode =
    (set_immediate_fn)(dlsym(_pcap_lib_handle, "pcap_set_immediate_mode"));
#endif

using namespace node;
using namespace v8;
using namespace std;

void emit_packet( uv_async_t* async_obj);
static void proc_read_packets(void* data);

class pkt {
    public:
    int len;
    u_char *payload;

    pkt(int size, u_char * data) {
        len = size;
        payload = new u_char[len]();
        std::memcpy(payload, data, len);
    };
    ~pkt() {
        free(payload);
    }
};

static Nan::Persistent<FunctionTemplate> constructor;
static Nan::Persistent<String> emit_symbol;
static Nan::Persistent<String> packet_symbol;

void SetAddrStringHelper(const char* key,
                         sockaddr *addr,
                         Local<Object> Address) {
  if (key && addr) {
    char dst_addr[INET6_ADDRSTRLEN + 1] = {0};
    char* src = 0;
    socklen_t size = 0;
    if (addr->sa_family == AF_INET) {
      struct sockaddr_in* saddr = (struct sockaddr_in*) addr;
      src = (char*) &(saddr->sin_addr);
      size = INET_ADDRSTRLEN;
    } else {
      struct sockaddr_in6* saddr6 = (struct sockaddr_in6*) addr;
      src = (char*) &(saddr6->sin6_addr);
      size = INET6_ADDRSTRLEN;
    }
    const char* address = inet_ntop(addr->sa_family, src, dst_addr, size);
    if (address == NULL)
      Address->Set(Nan::New<String>(key).ToLocalChecked(), Nan::Undefined());
    else {
      Address->Set(Nan::New<String>(key).ToLocalChecked(),
                   Nan::New<String>(address).ToLocalChecked());
    }
  }
}

class Pcap : public Nan::ObjectWrap {
  public:
    Nan::Persistent<Function> Emit;
    bool closing;
    bool handling_packets;
    unsigned int tv_sec;
    unsigned int tv_usec;

    uv_async_t async;
    pcap_t *pcap_handle;

    char *buffer_data;
    size_t buffer_length;

    uv_thread_t proc_r_p;
    uv_rwlock_t queueLock;
    queue<pkt*> q;

    pcap_send_queue* sendQueue;
    

    Pcap() {
      pcap_handle = NULL;
      buffer_data = NULL;
      buffer_length = 0;
      closing = false;
      handling_packets = false;
      tv_sec = 0;
      tv_usec = 0;
      sendQueue = NULL;
      
    }

    ~Pcap() {
      close();
      Emit.Reset();
    }

    bool close() {
      if (pcap_handle && !closing) {
        closing = true;
        cleanup();
        return true;
      }
      return false;
    }

    void cleanup() {
      if (pcap_handle && !handling_packets) {
        if (sendQueue != NULL) {
          pcap_sendqueue_destroy(sendQueue);
          sendQueue = NULL;
        }
        pcap_close(pcap_handle);
        pcap_handle = NULL;
        buffer_data = NULL;
        buffer_length = 0;
        Unref();
      }
    }

    // static void EmitPacket(u_char* user,
    //                        const struct pcap_pkthdr* pkt_hdr,
    //                        const u_char* pkt_data) {
    //   Nan::HandleScope scope;
    //   Pcap *obj = (Pcap*) user;

    //   size_t copy_len = pkt_hdr->caplen;
    //   bool truncated = false;
    //   if (copy_len > obj->buffer_length) {
    //     copy_len = obj->buffer_length;
    //     truncated = true;
    //   }
    //   memcpy(obj->buffer_data, pkt_data, copy_len);

    //   Local<Value> emit_argv[3] = {
    //     Nan::New<String>(packet_symbol),
    //     Nan::New<Number>(copy_len),
    //     Nan::New<Boolean>(truncated)
    //   };
    //   Nan::MakeCallback(
    //     Nan::New<Object>(obj->persistent()),
    //     Nan::New<Function>(obj->Emit),
    //     3,
    //     emit_argv
    //   );
    // }

   
    static void CALLBACK OnPacket(void* data, BOOLEAN didTimeout) {
      assert(!didTimeout);
      uv_async_t* async = (uv_async_t*)data;
      int r = uv_async_send(async);
      assert(r == 0);
    }
  
    static NAN_METHOD(New) {
      Nan::HandleScope scope;

      if (!info.IsConstructCall())
        return Nan::ThrowError("Use `new` to create instances of this object");

      Pcap *obj = new Pcap();
      obj->Wrap(info.This());

      obj->Emit.Reset(Local<Function>::Cast(
        Nan::New<Object>(obj->persistent())->Get(Nan::New<String>(emit_symbol))
      ));

      info.GetReturnValue().Set(info.This());
    }

    static NAN_METHOD(DestroyQueue) {
      Nan::HandleScope scope;
      Pcap *obj = Nan::ObjectWrap::Unwrap<Pcap>(info.This());
      if (obj->sendQueue != NULL) {
        pcap_sendqueue_destroy(obj->sendQueue);
        obj->sendQueue = NULL;
      }
    }

    static NAN_METHOD(AllocateQueue) {
      Nan::HandleScope scope;
      Pcap *obj = Nan::ObjectWrap::Unwrap<Pcap>(info.This());
      unsigned int memsize = 0;

      if (obj->sendQueue != NULL) {
        return Nan::ThrowError("The send queue has already been allocated.");
      }

      if (info.Length() == 0)
        return Nan::ThrowTypeError("the first parameter must be a positive integer");

      if (info.Length() >= 1) {
        if (!info[0]->IsUint32())
          return Nan::ThrowTypeError("memsize must be a positive integer");

        memsize = info[0]->Uint32Value();
      }

      obj->sendQueue = pcap_sendqueue_alloc(memsize);

      if (obj->sendQueue == NULL) {
        return Nan::ThrowError("Pcap could not allocate a queue.");
      }

      return;
    }

    static NAN_METHOD(TransmitQueue) {
      Nan::HandleScope scope;
      Pcap *obj = Nan::ObjectWrap::Unwrap<Pcap>(info.This());

      if (obj->sendQueue == NULL) {
        return Nan::ThrowError("TransmitQueue called before AllocateSendQueue");
      }

      unsigned int sendSize = obj->sendQueue->len;
      unsigned int bytesSent = 0;
      bytesSent = pcap_sendqueue_transmit(obj->pcap_handle, obj->sendQueue, 0);
      if (bytesSent != sendSize) {
        printf("Failed to send all packet data %d %d", bytesSent, sendSize);
        return Nan::ThrowError("Error: Some data not sent");
      } 

    }

    static NAN_METHOD(SendQueue) {
      Nan::HandleScope scope;
      Pcap *obj = Nan::ObjectWrap::Unwrap<Pcap>(info.This());
      pcap_pkthdr hdr ;
      size_t packetLength = 0;
      
      if (obj->sendQueue == NULL) {
        return Nan::ThrowError("SendQueue called before AllocateSendQueue");
      }

      if (info.Length() >= 2) {
        if (!info[1]->IsUint32()) {
          return Nan::ThrowTypeError("First parameter is length (positive integer`)");
        }
        if (!Buffer::HasInstance(info[0])) {
          return Nan::ThrowError("Missing Buffer parameter");
        }
      } else {
        return Nan::ThrowError("Wrong parameters in call to SendQueue");
      }

      #if NODE_MAJOR_VERSION == 0 && NODE_MINOR_VERSION < 10
      Local<Object> buffer_obj = info[0]->ToObject();
#else
      Local<Value> buffer_obj = info[0];
#endif
      packetLength = Buffer::Length(buffer_obj);
      if (packetLength > Buffer::Length(buffer_obj)) {
        return Nan::ThrowTypeError(
          "size must be smaller or equal to buffer length"
        );
      }
      obj->tv_usec += 1;
      if (obj->tv_usec == 1000000) {
        obj->tv_usec = 0;
        obj->tv_sec += 1;
      }
      hdr.ts.tv_sec = obj->tv_sec;
      hdr.ts.tv_usec = obj->tv_usec;
      hdr.caplen =(unsigned int)packetLength;
      hdr.len = (unsigned int)packetLength;
      // const u_char* ptr = (const u_char*)(Buffer::Data(buffer_obj));
      // printf("%03d %03d %03d %03d\n", ptr[38], ptr[39], ptr[40], ptr[41]);
      if (pcap_sendqueue_queue(obj->sendQueue, &hdr,  (const u_char*)(Buffer::Data(buffer_obj)))) {
        return Nan::ThrowError("Packet buffer too small");
      }

    }

    static NAN_METHOD(Send) {
      Nan::HandleScope scope;
      Pcap *obj = Nan::ObjectWrap::Unwrap<Pcap>(info.This());
      size_t buffer_size = 0;

      if (info.Length() == 0)
        return Nan::ThrowTypeError("the first parameter must be a buffer");

      if (!Buffer::HasInstance(info[0]))
        return Nan::ThrowTypeError("first parameter must be a buffer");

      if (info.Length() >= 2) {
        if (!info[1]->IsUint32())
          return Nan::ThrowTypeError("length must be a positive integer");

        buffer_size = info[1]->Uint32Value();
      }

#if NODE_MAJOR_VERSION == 0 && NODE_MINOR_VERSION < 10
      Local<Object> buffer_obj = info[0]->ToObject();
#else
      Local<Value> buffer_obj = info[0];
#endif
      if (info.Length() == 1)
        buffer_size = Buffer::Length(buffer_obj);
      else {
        if (buffer_size > Buffer::Length(buffer_obj)) {
          return Nan::ThrowTypeError(
            "size must be smaller or equal to buffer length"
          );
        }
      }

      if (pcap_sendpacket(obj->pcap_handle,
                          (const u_char*)(Buffer::Data(buffer_obj)),
                          (int)buffer_size) == -1) {
        return Nan::ThrowError(pcap_geterr(obj->pcap_handle));
      }

      return;
    }

    static NAN_METHOD(Open) {
      Nan::HandleScope scope;
      Pcap *obj = Nan::ObjectWrap::Unwrap<Pcap>(info.This());

      if (obj->pcap_handle)
        obj->close();

      if (info.Length() < 4)
        return Nan::ThrowTypeError("Expecting 4 arguments");

      if (!info[0]->IsString())
        return Nan::ThrowTypeError("device must be a string");

      if (!info[1]->IsString())
        return Nan::ThrowTypeError("filter must be a string");

      if (!info[2]->IsUint32())
        return Nan::ThrowTypeError("bufSize must be a positive integer");

      if (!Buffer::HasInstance(info[3]))
        return Nan::ThrowTypeError("buffer must be a Buffer");
        

      String::Utf8Value device(info[0]->ToString());
      String::Utf8Value filter(info[1]->ToString());
      int buffer_size = info[2]->Int32Value();
#if NODE_MAJOR_VERSION == 0 && NODE_MINOR_VERSION < 10
      Local<Object> buffer_obj = info[3]->ToObject();
#else
      Local<Value> buffer_obj = info[3];
#endif
      struct bpf_program fp;
      bpf_u_int32 mask;
      bpf_u_int32 net;
      char errbuf[PCAP_ERRBUF_SIZE];

      obj->buffer_data = Buffer::Data(buffer_obj);
      obj->buffer_length = Buffer::Length(buffer_obj);

      if (pcap_lookupnet((char*)*device,
                         &net,
                         &mask,
                         errbuf) == -1) {
        net = 0;
        mask = 0;
        fprintf(stderr, "Warning: %s - This may not actually work\n", errbuf);
      }

      obj->pcap_handle = pcap_create((char*)*device, errbuf);

      if (obj->pcap_handle == NULL)
        return Nan::ThrowError(errbuf);

      // 64KB is the max IPv4 packet size
      if (pcap_set_snaplen(obj->pcap_handle, 65535) != 0)
        return Nan::ThrowError("Unable to set snaplen");

      // Always use promiscuous mode
      if (pcap_set_promisc(obj->pcap_handle, 1) != 0)
        return Nan::ThrowError("Unable to set promiscuous mode");

      // Try to set buffer size. Sometimes the OS has a lower limit that it will
      // silently enforce.
      if (pcap_set_buffer_size(obj->pcap_handle, buffer_size) != 0)
        return Nan::ThrowError("Unable to set buffer size");

      // Set "timeout" on read, even though we are also setting nonblock below.
      // On Linux this is required.
      if (pcap_set_timeout(obj->pcap_handle, 1000) != 0)
        return Nan::ThrowError("Unable to set read timeout");

#if __linux__
      if (set_immediate_mode != NULL)
        set_immediate_mode(obj->pcap_handle, 1);
#endif

      if (pcap_activate(obj->pcap_handle) != 0)
        return Nan::ThrowError(pcap_geterr(obj->pcap_handle));

      if (pcap_setnonblock(obj->pcap_handle, 1, errbuf) == -1)
        return Nan::ThrowError(errbuf);

      if (filter.length() != 0) {
        if (pcap_compile(obj->pcap_handle, &fp, (char*)*filter, 1, net) == -1)
          return Nan::ThrowError(pcap_geterr(obj->pcap_handle));

        if (pcap_setfilter(obj->pcap_handle, &fp) == -1)
          return Nan::ThrowError(pcap_geterr(obj->pcap_handle));

        pcap_freecode(&fp);
      }

#if defined(__APPLE_CC__) || defined(__APPLE__)
      // Work around buffering bug in BPF on OSX 10.6 as of May 19, 2010
      // This may result in dropped packets under load because it disables the
      // (broken) buffer
      // http://seclists.org/tcpdump/2010/q1/110
      #include <net/bpf.h>
      int fd = pcap_get_selectable_fd(obj->pcap_handle);
      int v = 1;
      ioctl(fd, BIOCIMMEDIATE, &v);
      // TODO - check return value
#endif

      int link_type = pcap_datalink(obj->pcap_handle);

      Local<Value> ret;
      switch (link_type) {
        case DLT_NULL:
          ret = Nan::New<String>("NULL").ToLocalChecked();
          break;
        case DLT_EN10MB: // most wifi interfaces pretend to be "ethernet"
          ret =  Nan::New<String>("ETHERNET").ToLocalChecked();
          break;
        case DLT_IEEE802_11_RADIO: // 802.11 "monitor mode"
          ret = Nan::New<String>("IEEE802_11_RADIO").ToLocalChecked();
          break;
        case DLT_LINUX_SLL: // "Linux cooked-mode capture"
          ret = Nan::New<String>("LINKTYPE_LINUX_SLL").ToLocalChecked();
          break;
        case DLT_RAW: // "raw IP"
          ret = Nan::New<String>("RAW").ToLocalChecked();
          break;
        default:
          snprintf(errbuf, PCAP_ERRBUF_SIZE, "Unknown linktype %d", link_type);
          ret = Nan::New<String>(errbuf).ToLocalChecked();
          break;
      }
    
     
      uv_thread_create(&obj->proc_r_p, proc_read_packets, (void*)obj);


        uv_loop_t *loop = uv_default_loop();
        uv_rwlock_init(&obj->queueLock);
        uv_async_init(loop, &obj->async, emit_packet);


      obj->Ref();
      info.GetReturnValue().Set(ret);
    }

#ifdef _WIN32
    static NAN_METHOD(WIN_SetMin) {
      Nan::HandleScope scope;
      Pcap *obj = Nan::ObjectWrap::Unwrap<Pcap>(info.This());

      if (info.Length() < 1)
        return Nan::ThrowTypeError("missing min bytes value");

      if (!info[0]->IsUint32())
        return Nan::ThrowTypeError("min bytes must be a positive number");

      if (obj->pcap_handle == NULL)
        return Nan::ThrowError("Not currently capturing/open");

      if (pcap_setmintocopy(obj->pcap_handle, info[0]->Uint32Value()) != 0)
        return Nan::ThrowError("Unable to set min bytes");

      return;
    }
#endif

    static NAN_METHOD(Close) {
      Nan::HandleScope scope;
      Pcap *obj = Nan::ObjectWrap::Unwrap<Pcap>(info.This());

      info.GetReturnValue().Set(Nan::New<Boolean>(obj->close()));
    }

    static void Initialize(Handle<Object> target) {
      Nan::HandleScope scope;

      Local<FunctionTemplate> tpl = Nan::New<FunctionTemplate>(New);

      constructor.Reset(tpl);
      tpl->InstanceTemplate()->SetInternalFieldCount(1);
      tpl->SetClassName(Nan::New<String>("Cap").ToLocalChecked());

      Nan::SetPrototypeMethod(tpl, "allocateQueue", AllocateQueue);
      Nan::SetPrototypeMethod(tpl, "sendQueue", SendQueue);
      Nan::SetPrototypeMethod(tpl, "transmitQueue", TransmitQueue);
      Nan::SetPrototypeMethod(tpl, "destroyQueue", DestroyQueue);
      Nan::SetPrototypeMethod(tpl, "send", Send);
      Nan::SetPrototypeMethod(tpl, "open", Open);
      Nan::SetPrototypeMethod(tpl, "close", Close);
#ifdef _WIN32
      Nan::SetPrototypeMethod(tpl, "setMinBytes", WIN_SetMin);
#endif

      emit_symbol.Reset(Nan::New<String>("emit").ToLocalChecked());
      packet_symbol.Reset(Nan::New<String>("packet").ToLocalChecked());

      target->Set(Nan::New<String>("Cap").ToLocalChecked(), tpl->GetFunction());
    }
};

 void emit_packet( uv_async_t* async_obj) {
        Pcap* npcap = (Pcap*) async_obj->data;
        pkt* packet;
        size_t copy_len;
        bool truncated;

        uv_rwlock_wrlock(&npcap->queueLock);
        while(npcap->q.size()) {
            truncated = false;
            packet = (pkt *) npcap->q.front();
            npcap->q.pop();
            copy_len = packet->len;
            if (copy_len > npcap->buffer_length) {
                copy_len = npcap->buffer_length;
                truncated = true;
            }
            memcpy(npcap->buffer_data, packet->payload, copy_len);

            Local<Value> emit_argv[3] = {
                Nan::New<String>(packet_symbol),
                Nan::New<Number>(copy_len),
                Nan::New<Boolean>(truncated)
            };
            Nan::MakeCallback(
                Nan::New<Object>(npcap->persistent()),
                Nan::New<Function>(npcap->Emit),
                3,
                emit_argv
            );
            delete packet;
        }
        uv_rwlock_wrunlock(&npcap->queueLock);
        
    }

    static void proc_read_packets(void* data) {

      Pcap *obj = (Pcap*)data;
      pcap_pkthdr *header;
      const u_char *pkt_data;
      int next_ret;

      if (obj->closing)
        return obj->cleanup();

      obj->handling_packets = true;

      do {
        next_ret = pcap_next_ex(obj->pcap_handle,
                                     &header,
                                     &pkt_data);
        switch(next_ret) {
            case 1: 
                uv_rwlock_wrlock(&obj->queueLock);
                obj->q.push(new pkt(header->len, (u_char*) pkt_data));
                uv_rwlock_wrunlock(&obj->queueLock);
                uv_async_send(&obj->async); 
                break;
            case 0: break; // timeout reached, no op
            case -1: break; // error, no op
            case -2: break; // eof capture file (should not happen)
        }
      } while (next_ret >= 0 && !obj->closing);

      obj->handling_packets = false;
      if (obj->closing)
        obj->cleanup();
    }

static NAN_METHOD(ListDevices) {
  Nan::HandleScope scope;

  char errbuf[PCAP_ERRBUF_SIZE];
  pcap_if_t *alldevs = NULL, *cur_dev;
  pcap_addr_t *cur_addr;
  int i, j, af;

  Local<Object> Dev;
  Local<Object> Address;
  Local<Array> DevsArray;
  Local<Array> AddrArray;

  if (pcap_findalldevs(&alldevs, errbuf) == -1)
    return Nan::ThrowError(errbuf);

  DevsArray = Nan::New<Array>();

  for (i = 0, cur_dev = alldevs;
       cur_dev != NULL;
       cur_dev = cur_dev->next, ++i) {
    Dev = Nan::New<Object>();
    AddrArray = Nan::New<Array>();

    Dev->Set(Nan::New<String>("name").ToLocalChecked(),
             Nan::New<String>(cur_dev->name).ToLocalChecked());
    if (cur_dev->description != NULL) {
      Dev->Set(Nan::New<String>("description").ToLocalChecked(),
               Nan::New<String>(cur_dev->description).ToLocalChecked());
    }

    for (j = 0, cur_addr = cur_dev->addresses;
         cur_addr != NULL;
         cur_addr = cur_addr->next) {
      if (cur_addr->addr) {
        af = cur_addr->addr->sa_family;
        if (af == AF_INET || af == AF_INET6) {
          Address = Nan::New<Object>();
          SetAddrStringHelper("addr", cur_addr->addr, Address);
          SetAddrStringHelper("netmask", cur_addr->netmask, Address);
          SetAddrStringHelper("broadaddr", cur_addr->broadaddr, Address);
          SetAddrStringHelper("dstaddr", cur_addr->dstaddr, Address);
          AddrArray->Set(Nan::New<Integer>(j++), Address);
        }
      }
    }
      
    Dev->Set(Nan::New<String>("addresses").ToLocalChecked(), AddrArray);

    if (cur_dev->flags & PCAP_IF_LOOPBACK) {
      Dev->Set(Nan::New<String>("flags").ToLocalChecked(),
               Nan::New<String>("PCAP_IF_LOOPBACK").ToLocalChecked());
    }

    DevsArray->Set(Nan::New<Integer>(i), Dev);
  }

  if (alldevs)
    pcap_freealldevs(alldevs);

  info.GetReturnValue().Set(DevsArray);
}

static NAN_METHOD(FindDevice) {
  Nan::HandleScope scope;

  Local<Value> ret;
  char errbuf[PCAP_ERRBUF_SIZE];
  char name4[INET_ADDRSTRLEN];
  char name6[INET6_ADDRSTRLEN];
  char *ip = NULL;
  pcap_if_t *alldevs = NULL, *dev;
  pcap_addr_t *addr;
  bool found = false;

  if (pcap_findalldevs(&alldevs, errbuf) == -1)
    return Nan::ThrowError(errbuf);

  if (info.Length() > 0) { 
    if (!info[0]->IsString())
      return Nan::ThrowTypeError("Expected string for IP");
    Nan::Utf8String ipstr(info[0]);
    ip = (char*)malloc(strlen(*ipstr) + 1);
    strcpy(ip, *ipstr);
  }

  for (dev = alldevs; dev != NULL; dev = dev->next) {
    if (dev->addresses != NULL) {
      for (addr = dev->addresses; addr != NULL; addr = addr->next) {
        if (addr->addr->sa_family == AF_INET
            || addr->addr->sa_family == AF_INET6) {
          if (ip) {
            if (addr->addr->sa_family == AF_INET) {
              inet_ntop(AF_INET,
                        (char*)&(((struct sockaddr_in*)(addr->addr))->sin_addr),
                        name4, INET_ADDRSTRLEN);
              if (strcmp(ip, name4) != 0)
                continue;
            } else if (addr->addr->sa_family == AF_INET6) {
              inet_ntop(AF_INET6,
                        (char*)&(
                          ((struct sockaddr_in6*)(addr->addr))->sin6_addr
                        ),
                        name6, INET6_ADDRSTRLEN);
              if (strcmp(ip, name6) != 0)
                continue;
            }
          }
          ret = Nan::New<String>(dev->name).ToLocalChecked();
          found = true;
          break;
        }
      }
      if (found)
        break;
    }
  }

  if (alldevs)
    pcap_freealldevs(alldevs);

  if (ip)
    free(ip);

  info.GetReturnValue().Set(ret);
}

extern "C" {
  void init(Handle<Object> target) {
    Nan::HandleScope scope;
    Pcap::Initialize(target);
    target->Set(Nan::New<String>("findDevice").ToLocalChecked(),
                Nan::New<FunctionTemplate>(FindDevice)->GetFunction());
    target->Set(Nan::New<String>("deviceList").ToLocalChecked(),
                Nan::New<FunctionTemplate>(ListDevices)->GetFunction());
  }

  NODE_MODULE(cap, init);
}
