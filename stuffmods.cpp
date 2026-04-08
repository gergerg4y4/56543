#include <jni.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <android/log.h>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>

#define TAG "stuffmods"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static void* base = nullptr;
static void* rva(uint64_t o){ return (uint8_t*)base+o; }
template<typename T> static T    rp(void* p,size_t o){return *(T*)((uint8_t*)p+o);}
template<typename T> static void wp(void* p,size_t o,T v){*(T*)((uint8_t*)p+o)=v;}

// ── IL2CPP exports ────────────────────────────────────────────────────────────
static void*(*_strNew)(const char*)                             = nullptr;
static void*(*_classGetType)(void*)                            = nullptr;
static void*(*_typeGetObject)(void*)                           = nullptr;
static void*(*_domainGet)()                                    = nullptr;
static void*(*_asmOpen)(void*,const char*)                     = nullptr;
static void*(*_asmGetImage)(void*)                             = nullptr;
static void*(*_classFromName)(void*,const char*,const char*)   = nullptr;

// ── Unity function pointers ───────────────────────────────────────────────────
typedef void*(*CamGetMain_t)();
typedef void*(*GOCreatePrim_t)(int);
typedef void*(*GOAddComp_t)(void*,void*);
typedef void*(*CompGetTrans_t)(void*);
typedef void (*TransSetParent_t)(void*,void*,bool);
typedef void (*TransSetLocalPos_t)(void*,float,float,float);
typedef void (*TransSetLocalScale_t)(void*,float,float,float);
typedef void (*TMPSetText_t)(void*,void*);
typedef void (*Destroy_t)(void*);
typedef void*(*GOGetComp_t)(void*,void*);

static CamGetMain_t     _camMain    = nullptr;
static GOCreatePrim_t   _createPrim = nullptr;
static GOAddComp_t      _addComp    = nullptr;
static CompGetTrans_t   _getTrans   = nullptr;
static TransSetParent_t _setParent  = nullptr;
static TransSetLocalPos_t _setLocalPos = nullptr;
static TransSetLocalScale_t _setLocalScale = nullptr;
static TMPSetText_t     _setText    = nullptr;
static Destroy_t        _destroy    = nullptr;
static GOGetComp_t      _getComp    = nullptr;
static void*            _tmpClass   = nullptr;
static void*            _mrClass    = nullptr;

// ── Mod state ─────────────────────────────────────────────────────────────────
static bool g_infAmmo   = true;
static bool g_rapidFire = true;
static bool g_maxCurr   = true;
static int  g_frame     = 0;

struct Item { const char* name; bool* val; };
static Item g_items[] = {
    {"Inf Ammo",     &g_infAmmo  },
    {"Rapid Fire",   &g_rapidFire},
    {"Max Currency", &g_maxCurr  },
};
static const int N = 3;

// ── HUD ───────────────────────────────────────────────────────────────────────
static void* g_lines[N+2] = {};
static bool  g_hudBuilt   = false;

static void* makeLine(void* camTrans, float x, float y, float z, float scale){
    void* go = _createPrim(3);
    if(!go) return nullptr;
    void* t = _getTrans(go);
    if(!t) return nullptr;
    _setParent(t, camTrans, false);
    _setLocalPos(t, x, y, z);
    _setLocalScale(t, scale, scale, scale);
    // Remove MeshRenderer
    if(_destroy&&_getComp&&_mrClass){
        void* mrType = _classGetType(_mrClass);
        void* mrObj  = _typeGetObject(mrType);
        void* mr = _getComp(go, mrObj);
        if(mr) _destroy(mr);
    }
    // Add TextMeshPro component
    void* tmpType = _classGetType(_tmpClass);
    void* tmpObj  = _typeGetObject(tmpType);
    void* tm = _addComp(go, tmpObj);
    LOG("makeLine y=%.2f tm=%p", y, tm);
    return tm;
}

static bool buildHUD(){
    LOG("buildHUD start");
    if(!_camMain||!_createPrim||!_addComp||!_getTrans||
       !_setParent||!_setLocalPos||!_setLocalScale||!_setText||!_tmpClass){
        LOG("buildHUD: missing pointers camMain=%p createPrim=%p addComp=%p setText=%p tmpClass=%p",
            (void*)_camMain,(void*)_createPrim,(void*)_addComp,(void*)_setText,_tmpClass);
        return false;
    }
    void* cam = _camMain();
    LOG("buildHUD cam=%p", cam);
    if(!cam) return false;
    void* camT = _getTrans(cam);
    LOG("buildHUD camT=%p", camT);
    if(!camT) return false;

    float z=0.5f, x=0.18f, startY=0.12f, step=0.025f, scale=0.002f;
    for(int i=0;i<N+2;i++){
        g_lines[i] = makeLine(camT, x, startY - i*step, z, scale);
    }
    if(!g_lines[0]){ LOG("buildHUD: line 0 failed"); return false; }
    _setText(g_lines[0], _strNew("=== StuffMods ==="));
    _setText(g_lines[N+1], _strNew("X=next  A=toggle"));
    LOG("buildHUD OK");
    return true;
}

static void updateHUD(){
    for(int i=0;i<N;i++){
        if(!g_lines[i+1]) continue;
        char buf[64];
        snprintf(buf,sizeof(buf),"%s [%s]",g_items[i].name,*g_items[i].val?"ON":"OFF");
        _setText(g_lines[i+1], _strNew(buf));
    }
}

// ── Hook infra ────────────────────────────────────────────────────────────────
typedef int(*DobbyHook_t)(void*,void*,void**);
static DobbyHook_t _dobby=nullptr;
static bool hook(void* tgt,void* rep,void** orig){
    if(!_dobby){
        _dobby=(DobbyHook_t)dlsym(RTLD_DEFAULT,"DobbyHook");
        if(!_dobby)_dobby=(DobbyHook_t)dlsym(RTLD_DEFAULT,"A64HookFunction");
        LOG("dobby=%p",(void*)_dobby);
    }
    if(_dobby){int r=_dobby(tgt,rep,orig);LOG("hook %p -> %p r=%d orig=%p",tgt,rep,r,orig?*orig:nullptr);return r==0;}
    // manual trampoline
    uintptr_t pg=(uintptr_t)tgt&~0xFFFull;
    if(mprotect((void*)pg,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC)!=0){LOG("mprotect failed");return false;}
    if(orig){
        uint8_t* tr=(uint8_t*)mmap(nullptr,32,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(tr!=MAP_FAILED){
            memcpy(tr,tgt,16);
            uint32_t j[4]={0x58000051,0xD61F0220,0,0};
            uintptr_t c=(uintptr_t)tgt+16;memcpy(&j[2],&c,8);
            memcpy(tr+16,j,16);__builtin___clear_cache((char*)tr,(char*)tr+32);
            *orig=tr;
        }
    }
    uint32_t p[4]={0x58000051,0xD61F0220,0,0};
    memcpy(&p[2],&rep,8);memcpy(tgt,p,16);
    __builtin___clear_cache((char*)tgt,(char*)tgt+16);
    LOG("manual hook %p -> %p",tgt,rep);
    return true;
}

// ── Weapon hooks ──────────────────────────────────────────────────────────────
typedef void(*ShamUpd_t)(void*); static ShamUpd_t _shamOrig=nullptr;
static void shamHook(void* s){
    if(_shamOrig)_shamOrig(s);if(!s)return;
    if(g_infAmmo){int32_t m=rp<int32_t>(s,0x88);if(m>0&&m<9999){wp<int32_t>(s,0x98,m);wp<uint8_t>(s,0x9C,0);}}
    if(g_rapidFire){wp<float>(s,0x8C,0.f);wp<uint8_t>(s,0x9C,0);}
}
typedef void(*ShotUpd_t)(void*); static ShotUpd_t _shotOrig=nullptr;
static void shotHook(void* s){
    if(_shotOrig)_shotOrig(s);if(!s)return;
    if(g_infAmmo||g_rapidFire){wp<uint8_t>(s,0x40,1);wp<uint8_t>(s,0x98,0);wp<int32_t>(s,0x9C,0);}
}

// ── CGM.LateUpdate hook ───────────────────────────────────────────────────────
typedef void(*CGMLate_t)(void*); static CGMLate_t _cgmOrig=nullptr;
static void cgmHook(void* self){
    if(_cgmOrig)_cgmOrig(self);
    g_frame++;
    if(g_frame==1) LOG("cgmHook firing! frame=1");
    if(g_maxCurr&&g_frame%300==0){
        wp<int32_t>(self,0xD0,999999);
        wp<int32_t>(self,0xE4,999999);
    }
    if(!g_hudBuilt&&g_frame==120){
        LOG("attempting buildHUD frame=120");
        g_hudBuilt=buildHUD();
        LOG("buildHUD=%d",g_hudBuilt);
    }
    if(g_hudBuilt&&g_frame%30==0) updateHUD();
}

// ── Setup ─────────────────────────────────────────────────────────────────────
static void* setup(void*){
    sleep(20);

    char path[512]={};
    {FILE* m=fopen("/proc/self/maps","r");char l[512];
    while(m&&fgets(l,sizeof(l),m)){
        if(strstr(l,"libil2cpp.so")&&strstr(l,"r-xp")){
            uint64_t b=0;sscanf(l,"%llx",(unsigned long long*)&b);base=(void*)b;
            char* p=strrchr(l,' ');if(!p)p=strrchr(l,'\t');
            if(p){strncpy(path,p+1,sizeof(path)-1);path[strcspn(path,"\n")]=0;}
            LOG("base=%p path=%s",base,path);break;
        }
    }if(m)fclose(m);}
    if(!base){LOG("no libil2cpp");return nullptr;}

    void* h=dlopen(path[0]?path:"libil2cpp.so",RTLD_NOLOAD|RTLD_NOW|RTLD_GLOBAL);
    void* src=h?h:RTLD_DEFAULT;
    LOG("il2cpp handle=%p",h);
    _strNew      =(decltype(_strNew))      dlsym(src,"il2cpp_string_new");
    _classGetType=(decltype(_classGetType))dlsym(src,"il2cpp_class_get_type");
    _typeGetObject=(decltype(_typeGetObject))dlsym(src,"il2cpp_type_get_object");
    _domainGet   =(decltype(_domainGet))   dlsym(src,"il2cpp_domain_get");
    _asmOpen     =(decltype(_asmOpen))     dlsym(src,"il2cpp_domain_assembly_open");
    _asmGetImage =(decltype(_asmGetImage)) dlsym(src,"il2cpp_assembly_get_image");
    _classFromName=(decltype(_classFromName))dlsym(src,"il2cpp_class_from_name");
    typedef void**(*GetAsms_t)(void*,size_t*);
    auto _getAsms=(GetAsms_t)dlsym(src,"il2cpp_domain_get_assemblies");
    LOG("strNew=%p getAsms=%p",(void*)_strNew,(void*)_getAsms);

    // Unity RVAs
    _camMain    =(CamGetMain_t)    rva(0x4682E6C);
    _createPrim =(GOCreatePrim_t)  rva(0x46B3318);
    _addComp    =(GOAddComp_t)     rva(0x46B36CC);
    _getTrans   =(CompGetTrans_t)  rva(0x46B07F0);
    _setParent  =(TransSetParent_t)rva(0x46BD8DC);
    _setLocalPos=(TransSetLocalPos_t)rva(0x46BC5CC);
    _setLocalScale=(TransSetLocalScale_t)rva(0x46BCE28);
    _setText    =(TMPSetText_t)    rva(0x449D348);
    _destroy    =(Destroy_t)       rva(0x46B7550);
    _getComp    =(GOGetComp_t)     rva(0x46B08E0);

    // Find TextMeshPro + MeshRenderer classes
    if(_domainGet&&_getAsms){
        void* dom=nullptr;
        for(int r=0;r<15&&!dom;r++){dom=_domainGet();if(!dom)sleep(1);}
        if(dom){
            size_t cnt=0;
            void** asms=_getAsms(dom,&cnt);
            LOG("scanning %zu assemblies",(size_t)cnt);
            for(size_t a=0;a<cnt;a++){
                void* img=_asmGetImage(asms[a]);if(!img)continue;
                if(!_tmpClass){
                    void* c=_classFromName(img,"TMPro","TextMeshPro");
                    if(c){_tmpClass=c;LOG("TextMeshPro at asm %zu",(size_t)a);}
                }
                if(!_mrClass){
                    void* c=_classFromName(img,"","MeshRenderer");
                    if(!c)c=_classFromName(img,"UnityEngine","MeshRenderer");
                    if(c){_mrClass=c;LOG("MeshRenderer at asm %zu",(size_t)a);}
                }
                if(_tmpClass&&_mrClass)break;
            }
        }
    }
    if(!_tmpClass)LOG("WARNING: TextMeshPro not found");

    // Install hooks
    sleep(2);
    bool r1=hook(rva(0x20AFB28),(void*)cgmHook, (void**)&_cgmOrig);
    LOG("CGM hook installed=%d orig=%p",r1,_cgmOrig);
    sleep(1);
    hook(rva(0x20CA130),(void*)shamHook,(void**)&_shamOrig);
    hook(rva(0x20E0304),(void*)shotHook,(void**)&_shotOrig);

    LOG("stuffmods ready - waiting for cgmHook to fire...");
    return nullptr;
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm,void*){
    (void)vm;
    LOG("stuffmods loaded!");
    pthread_t t;pthread_create(&t,nullptr,setup,nullptr);pthread_detach(t);
    return JNI_VERSION_1_6;
}
