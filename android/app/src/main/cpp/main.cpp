#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <android/log.h>
#include "vr4mac.h"
#include "PosePrediction.h"
static_assert(sizeof(VR4Tracking)==284, "TRACKING contract changed");
static_assert(sizeof(VR4VideoHeader)==17, "VIDEO contract changed");
#include <atomic>
#include <array>
#include <deque>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstring>
#include <mutex>
#include <thread>
#include <chrono>

static std::atomic<bool> stopping{false};
struct Haptic { int hand; float amplitude,duration,frequency; };
static std::mutex hapticMutex;
static std::deque<Haptic> haptics;
static void check(XrResult r, const char* operation) { if (XR_FAILED(r)) throw std::runtime_error(std::string(operation)+": "+std::to_string(r)); }
#define XR(call) check(call,#call)
static XrPosef identity() { XrPosef p{}; p.orientation.w=1; return p; }
struct Snapshot { XrTime displayTime; XrTime time; std::array<XrView,2> views; };
struct Client {
    JNIEnv* env; jobject activity; JavaVM* vm;
    bool stageSpace=true;
    XrInstance instance{}; XrSession session{}; XrSpace space{},headSpace{};
    XrActionSet actionSet{}; XrAction aim{},grip{},trigger{},squeeze{},stick{},vibrate{};
    std::array<XrAction,9> buttons{};
    std::array<XrPath,2> hands{};
    std::array<XrSpace,2> aimSpace{},gripSpace{};
    std::array<XrSwapchain,2> chains{};
    std::array<std::vector<XrSwapchainImageOpenGLESKHR>,2> images;
    std::array<XrViewConfigurationView,2> sizes{};
    EGLDisplay display=EGL_NO_DISPLAY; EGLContext context=EGL_NO_CONTEXT; EGLSurface eglSurface=EGL_NO_SURFACE;
    GLuint texture{},program{},fbo{},vao{};
    jmethodID prepare{},update{},send{},release{};
    jfloatArray transformArray{};
    std::array<float,16> transform{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    std::deque<Snapshot> history;
    PosePrediction prediction;
    XrTime predictionLogTime=0;
    Snapshot rendered{}; bool hasVideo=false, running=false, focused=false, refreshReported=false;
    XrPath path(const char* s) { XrPath p; XR(xrStringToPath(instance,s,&p)); return p; }
    XrAction action(const char* name, XrActionType type) {
        XrActionCreateInfo i{XR_TYPE_ACTION_CREATE_INFO}; i.actionType=type; i.countSubactionPaths=2; i.subactionPaths=hands.data();
        strncpy(i.actionName,name,sizeof(i.actionName)-1); strncpy(i.localizedActionName,name,sizeof(i.localizedActionName)-1);
        XrAction a; XR(xrCreateAction(actionSet,&i,&a)); return a;
    }
    void init() {
        env->GetJavaVM(&vm);
        PFN_xrInitializeLoaderKHR loader{};
        XR(xrGetInstanceProcAddr(XR_NULL_HANDLE,"xrInitializeLoaderKHR",reinterpret_cast<PFN_xrVoidFunction*>(&loader)));
        XrLoaderInitInfoAndroidKHR init{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR}; init.applicationVM=vm; init.applicationContext=activity;
        XR(loader(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&init)));
        uint32_t extensionCount=0;
        XR(xrEnumerateInstanceExtensionProperties(nullptr,0,&extensionCount,nullptr));
        std::vector<XrExtensionProperties> available(extensionCount);
        for(auto& e:available) e.type=XR_TYPE_EXTENSION_PROPERTIES;
        XR(xrEnumerateInstanceExtensionProperties(nullptr,extensionCount,&extensionCount,available.data()));
        bool refreshExtension=false;
        for(const auto& e:available) if(std::strcmp(e.extensionName,XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)==0) refreshExtension=true;
        for(const auto& e:available) if(std::strcmp(e.extensionName,XR_EXT_HAND_TRACKING_EXTENSION_NAME)==0) handExtension=true;
        std::vector<const char*> extensions{XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME};
        if(refreshExtension) extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        if(handExtension) extensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
        XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR}; android.applicationVM=vm; android.applicationActivity=activity;
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO}; info.next=&android; strcpy(info.applicationInfo.applicationName,"VR4Mac"); info.applicationInfo.apiVersion=XR_MAKE_VERSION(1,0,0); info.enabledExtensionCount=extensions.size(); info.enabledExtensionNames=extensions.data();
        XR(xrCreateInstance(&info,&instance));
        XrSystemGetInfo sysInfo{XR_TYPE_SYSTEM_GET_INFO}; sysInfo.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY; XrSystemId system;
        XR(xrGetSystem(instance,&sysInfo,&system));
        PFN_xrGetOpenGLESGraphicsRequirementsKHR requirementsFn{}; XR(xrGetInstanceProcAddr(instance,"xrGetOpenGLESGraphicsRequirementsKHR",reinterpret_cast<PFN_xrVoidFunction*>(&requirementsFn)));
        XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR}; XR(requirementsFn(instance,system,&requirements));
        display=eglGetDisplay(EGL_DEFAULT_DISPLAY); if (!eglInitialize(display,nullptr,nullptr)) throw std::runtime_error("eglInitialize failed");
        EGLint attributes[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
        EGLConfig config; EGLint count; if (!eglChooseConfig(display,attributes,&config,1,&count)||!count) throw std::runtime_error("No EGL config");
        EGLint ctx[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE}; context=eglCreateContext(display,config,EGL_NO_CONTEXT,ctx);
        EGLint surf[]={EGL_WIDTH,16,EGL_HEIGHT,16,EGL_NONE}; eglSurface=eglCreatePbufferSurface(display,config,surf);
        if (!eglMakeCurrent(display,eglSurface,eglSurface,context)) throw std::runtime_error("eglMakeCurrent failed");
        XrGraphicsBindingOpenGLESAndroidKHR graphics{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR}; graphics.display=display; graphics.config=config; graphics.context=context;
        XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO}; si.next=&graphics; si.systemId=system; XR(xrCreateSession(instance,&si,&session));
        float refreshRate=0;
        if(refreshExtension) {
            PFN_xrGetDisplayRefreshRateFB getRefresh{};
            if(XR_SUCCEEDED(xrGetInstanceProcAddr(instance,"xrGetDisplayRefreshRateFB",reinterpret_cast<PFN_xrVoidFunction*>(&getRefresh))) && getRefresh) {
                if(XR_FAILED(getRefresh(session,&refreshRate))) refreshRate=0;
            }
        }
        XrInstanceProperties runtime{XR_TYPE_INSTANCE_PROPERTIES};
        XR(xrGetInstanceProperties(instance,&runtime));
        __android_log_print(ANDROID_LOG_INFO,"VR4Mac","OpenXR runtime: %s; current refresh %.1f Hz",runtime.runtimeName,refreshRate);
        jclass activityClass=env->GetObjectClass(activity);
        jmethodID reportRefresh=env->GetMethodID(activityClass,"reportRefreshRate","(F)V");
        env->CallVoidMethod(activity,reportRefresh,refreshRate);
        refreshReported=refreshRate>=30 && refreshRate<=240;
        XrReferenceSpaceCreateInfo ri{XR_TYPE_REFERENCE_SPACE_CREATE_INFO}; ri.poseInReferenceSpace=identity(); ri.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_STAGE;
        if (XR_FAILED(xrCreateReferenceSpace(session,&ri,&space))) { stageSpace=false; ri.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL; XR(xrCreateReferenceSpace(session,&ri,&space)); }
        ri.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_VIEW; XR(xrCreateReferenceSpace(session,&ri,&headSpace));
        uint32_t n; XR(xrEnumerateViewConfigurationViews(instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,0,&n,nullptr));
        if(n!=2) throw std::runtime_error("Stereo views required"); for(auto& v:sizes) v.type=XR_TYPE_VIEW_CONFIGURATION_VIEW;
        XR(xrEnumerateViewConfigurationViews(instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&n,sizes.data()));
        uint32_t nf; XR(xrEnumerateSwapchainFormats(session,0,&nf,nullptr)); std::vector<int64_t> formats(nf); XR(xrEnumerateSwapchainFormats(session,nf,&nf,formats.data()));
        int64_t format=0; for(auto f:formats) if(f==GL_RGBA8) format=f;
        for(auto f:formats) if(f==GL_SRGB8_ALPHA8) format=f;
        if(!format) throw std::runtime_error("RGB swapchain unavailable");
        __android_log_print(ANDROID_LOG_INFO,"VR4Mac","Swapchain color format: %s; video RGB converted to linear",format==GL_SRGB8_ALPHA8?"sRGB8":"linear RGBA8");
        for(int eye=0;eye<2;eye++) {
            XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO}; ci.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT; ci.format=format; ci.sampleCount=1; ci.width=sizes[eye].recommendedImageRectWidth; ci.height=sizes[eye].recommendedImageRectHeight; ci.faceCount=ci.arraySize=ci.mipCount=1;
            XR(xrCreateSwapchain(session,&ci,&chains[eye])); XR(xrEnumerateSwapchainImages(chains[eye],0,&n,nullptr)); images[eye].resize(n); for(auto& image:images[eye]) image.type=XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
            XR(xrEnumerateSwapchainImages(chains[eye],n,&n,reinterpret_cast<XrSwapchainImageBaseHeader*>(images[eye].data())));
        }
        initActions(); initHands(); initGL();
        jclass cls=env->GetObjectClass(activity); prepare=env->GetMethodID(cls,"prepareVideo","(IIIZ)V"); update=env->GetMethodID(cls,"updateVideo","([F)J"); send=env->GetMethodID(cls,"sendTracking","([B)V"); release=env->GetMethodID(cls,"releaseVideo","()V");
        transformArray=env->NewFloatArray(16); env->CallVoidMethod(activity,prepare,(jint)texture,(jint)sizes[0].recommendedImageRectWidth,(jint)sizes[0].recommendedImageRectHeight,(jboolean)stageSpace);
    }
    // Hand tracking (XR_EXT_hand_tracking): joints ride along on the TRACKING packet while a hand is tracked.
    bool handExtension=false; std::array<XrHandTrackerEXT,2> trackers{};
    PFN_xrLocateHandJointsEXT locateJoints{}; PFN_xrDestroyHandTrackerEXT destroyTracker{};
    void initHands() {
        if(!handExtension) return;
        PFN_xrCreateHandTrackerEXT create{};
        xrGetInstanceProcAddr(instance,"xrCreateHandTrackerEXT",reinterpret_cast<PFN_xrVoidFunction*>(&create));
        xrGetInstanceProcAddr(instance,"xrLocateHandJointsEXT",reinterpret_cast<PFN_xrVoidFunction*>(&locateJoints));
        xrGetInstanceProcAddr(instance,"xrDestroyHandTrackerEXT",reinterpret_cast<PFN_xrVoidFunction*>(&destroyTracker));
        if(!create||!locateJoints) return;
        for(int hand=0;hand<2;hand++) {
            XrHandTrackerCreateInfoEXT ci{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT}; ci.hand=hand?XR_HAND_RIGHT_EXT:XR_HAND_LEFT_EXT; ci.handJointSet=XR_HAND_JOINT_SET_DEFAULT_EXT;
            if(XR_FAILED(create(session,&ci,&trackers[hand]))) trackers[hand]=XR_NULL_HANDLE;
        }
        __android_log_print(ANDROID_LOG_INFO,"VR4Mac","Hand tracking %s",trackers[0]?"on":"unavailable");
    }
    /// Appends VR4HandJoints[2] when either hand is tracked (controllers down).
    void handJoints(std::vector<uint8_t>& b,XrTime time) {
        if(!trackers[0]||!focused) return;
        std::vector<uint8_t> extra; bool any=false;
        for(int hand=0;hand<2;hand++) {
            std::array<XrHandJointLocationEXT,XR_HAND_JOINT_COUNT_EXT> joints{};
            XrHandJointLocationsEXT locs{XR_TYPE_HAND_JOINT_LOCATIONS_EXT}; locs.jointCount=XR_HAND_JOINT_COUNT_EXT; locs.jointLocations=joints.data();
            XrHandJointsLocateInfoEXT li{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT}; li.baseSpace=space; li.time=time;
            bool ok=trackers[hand] && XR_SUCCEEDED(locateJoints(trackers[hand],&li,&locs)) && locs.isActive;
            for(auto& j:joints) if((j.locationFlags&(XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))!=(XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) ok=false;
            put(extra,(uint32_t)(ok?1:0)); any|=ok;
            for(auto& j:joints) pose(extra,ok?j.pose:identity());
        }
        if(any) b.insert(b.end(),extra.begin(),extra.end());
    }
    void initActions() {
        hands={path("/user/hand/left"),path("/user/hand/right")};
        XrActionSetCreateInfo ai{XR_TYPE_ACTION_SET_CREATE_INFO}; strcpy(ai.actionSetName,"controllers"); strcpy(ai.localizedActionSetName,"Controllers"); XR(xrCreateActionSet(instance,&ai,&actionSet));
        aim=action("aim",XR_ACTION_TYPE_POSE_INPUT); grip=action("grip",XR_ACTION_TYPE_POSE_INPUT); trigger=action("trigger",XR_ACTION_TYPE_FLOAT_INPUT); squeeze=action("squeeze",XR_ACTION_TYPE_FLOAT_INPUT); stick=action("stick",XR_ACTION_TYPE_VECTOR2F_INPUT); vibrate=action("haptic",XR_ACTION_TYPE_VIBRATION_OUTPUT);
        const char* names[]={"a","b","x","y","menu","stick_click","trigger_touch","thumb_touch","stick_touch"}; for(int i=0;i<9;i++) buttons[i]=action(names[i],XR_ACTION_TYPE_BOOLEAN_INPUT);
        std::vector<XrActionSuggestedBinding> bindings;
        auto bind=[&](XrAction a,const std::string& p){ bindings.push_back({a,path(p.c_str())}); };
        for(int hand=0;hand<2;hand++) {
            std::string base=hand?"/user/hand/right":"/user/hand/left";
            bind(aim,base+"/input/aim/pose"); bind(grip,base+"/input/grip/pose"); bind(trigger,base+"/input/trigger/value"); bind(squeeze,base+"/input/squeeze/value"); bind(stick,base+"/input/thumbstick"); bind(vibrate,base+"/output/haptic");
            bind(buttons[5],base+"/input/thumbstick/click"); bind(buttons[6],base+"/input/trigger/touch"); bind(buttons[7],base+"/input/thumbrest/touch"); bind(buttons[8],base+"/input/thumbstick/touch");
            if(hand) { bind(buttons[0],base+"/input/a/click"); bind(buttons[1],base+"/input/b/click"); } else { bind(buttons[2],base+"/input/x/click"); bind(buttons[3],base+"/input/y/click"); bind(buttons[4],base+"/input/menu/click"); }
            for(const char* t: hand ? std::initializer_list<const char*>{"a","b"} : std::initializer_list<const char*>{"x","y"}) bind(buttons[7],base+"/input/"+t+"/touch");   // thumb resting on a face button counts as thumb touch
            XrActionSpaceCreateInfo sci{XR_TYPE_ACTION_SPACE_CREATE_INFO}; sci.subactionPath=hands[hand]; sci.poseInActionSpace=identity(); sci.action=aim; XR(xrCreateActionSpace(session,&sci,&aimSpace[hand])); sci.action=grip; XR(xrCreateActionSpace(session,&sci,&gripSpace[hand]));
        }
        XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING}; suggested.interactionProfile=path("/interaction_profiles/oculus/touch_controller"); suggested.countSuggestedBindings=bindings.size(); suggested.suggestedBindings=bindings.data(); XR(xrSuggestInteractionProfileBindings(instance,&suggested));
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO}; attach.countActionSets=1; attach.actionSets=&actionSet; XR(xrAttachSessionActionSets(session,&attach));
    }
    GLuint shader(GLenum kind,const char* source) { GLuint s=glCreateShader(kind); glShaderSource(s,1,&source,nullptr); glCompileShader(s); GLint ok; glGetShaderiv(s,GL_COMPILE_STATUS,&ok); if(!ok) throw std::runtime_error("Shader failed"); return s; }
    void initGL() {
        const char* vs="#version 300 es\n out vec2 uv; void main(){ vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2); uv=p; gl_Position=vec4(p*2.0-1.0,0,1);}";
        const char* fs="#version 300 es\n#extension GL_OES_EGL_image_external_essl3 : require\nprecision highp float; uniform samplerExternalOES video; uniform mat4 transform; uniform float eye; in vec2 uv; out vec4 color; void main(){vec2 stereo=vec2((uv.x+eye)*0.5,uv.y); vec4 encoded=texture(video,(transform*vec4(stereo,0,1)).xy); vec3 rgb=clamp(encoded.rgb,0.0,1.0); vec3 linearRGB=mix(pow((rgb+0.055)/1.055,vec3(2.4)),rgb/12.92,lessThanEqual(rgb,vec3(0.04045))); color=vec4(linearRGB,encoded.a);}";
        GLuint v=shader(GL_VERTEX_SHADER,vs),f=shader(GL_FRAGMENT_SHADER,fs); program=glCreateProgram(); glAttachShader(program,v); glAttachShader(program,f); glLinkProgram(program); glDeleteShader(v); glDeleteShader(f); GLint linked; glGetProgramiv(program,GL_LINK_STATUS,&linked); if(!linked) throw std::runtime_error("Shader link failed");
        glGenTextures(1,&texture); glBindTexture(GL_TEXTURE_EXTERNAL_OES,texture); glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_MAG_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE); glGenFramebuffers(1,&fbo); glGenVertexArrays(1,&vao);
    }
    template<class T> static void put(std::vector<uint8_t>& bytes,const T& value) { const auto* p=reinterpret_cast<const uint8_t*>(&value); bytes.insert(bytes.end(),p,p+sizeof(value)); }
    static void pose(std::vector<uint8_t>& b,const XrPosef& p) { put(b,p.position.x);put(b,p.position.y);put(b,p.position.z);put(b,p.orientation.x);put(b,p.orientation.y);put(b,p.orientation.z);put(b,p.orientation.w); }
    static bool valid(const XrSpaceLocation& l) { return (l.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))==(XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT); }
    void tracking(const Snapshot& snapshot) {
        XrActiveActionSet active{actionSet,XR_NULL_PATH}; XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO}; sync.countActiveActionSets=1; sync.activeActionSets=&active; if(focused) xrSyncActions(session,&sync);
        std::vector<uint8_t> b; put(b,(uint64_t)snapshot.time);
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION}; xrLocateSpace(headSpace,space,snapshot.time,&head); pose(b,valid(head)?head.pose:identity());
        for(auto& v:snapshot.views) { pose(b,v.pose); put(b,v.fov.angleLeft);put(b,v.fov.angleRight);put(b,v.fov.angleUp);put(b,v.fov.angleDown); }
        for(int hand=0;hand<2;hand++) {
            XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO}; get.subactionPath=hands[hand]; get.action=aim;
            XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE}; xrGetActionStatePose(session,&get,&state);
            XrSpaceLocation a{XR_TYPE_SPACE_LOCATION},g{XR_TYPE_SPACE_LOCATION}; xrLocateSpace(aimSpace[hand],space,snapshot.time,&a); xrLocateSpace(gripSpace[hand],space,snapshot.time,&g);
            uint32_t flags=focused && state.isActive?1:0; if(flags && valid(a)&&valid(g)) flags|=2; put(b,flags);
            uint32_t bits=0; for(int i=0;i<9;i++) { get.action=buttons[i]; XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN}; if(focused && XR_SUCCEEDED(xrGetActionStateBoolean(session,&get,&s)) && s.isActive && s.currentState) bits|=1u<<i; } put(b,bits);
            pose(b,valid(a)?a.pose:identity()); pose(b,valid(g)?g.pose:identity());
            for(auto action:{trigger,squeeze}) { get.action=action; XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT}; xrGetActionStateFloat(session,&get,&s); put(b,focused && s.isActive?s.currentState:0.0f); }
            get.action=stick; XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F}; xrGetActionStateVector2f(session,&get,&s); put(b,focused && s.isActive?s.currentState.x:0.0f);put(b,focused && s.isActive?s.currentState.y:0.0f);
        }
        if(b.size()!=sizeof(VR4Tracking)) throw std::runtime_error("TRACKING layout mismatch");
        handJoints(b,snapshot.time);
        jbyteArray packet=env->NewByteArray(b.size()); env->SetByteArrayRegion(packet,0,b.size(),reinterpret_cast<jbyte*>(b.data())); env->CallVoidMethod(activity,send,packet); env->DeleteLocalRef(packet);
        std::deque<Haptic> pending; { std::lock_guard<std::mutex> lock(hapticMutex); pending.swap(haptics); }
        for(auto& h:pending) { XrHapticActionInfo ai{XR_TYPE_HAPTIC_ACTION_INFO}; ai.action=vibrate; ai.subactionPath=hands[h.hand]; XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION}; v.amplitude=h.amplitude;v.duration=(XrDuration)(h.duration*1e9);v.frequency=h.frequency; if(focused) xrApplyHapticFeedback(session,&ai,reinterpret_cast<XrHapticBaseHeader*>(&v)); }
    }
    void loop() {
        while(!stopping) {
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            while(xrPollEvent(instance,&event)==XR_SUCCESS) {
                if(event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                    auto state=reinterpret_cast<XrEventDataSessionStateChanged*>(&event)->state;
                    focused=state==XR_SESSION_STATE_FOCUSED;
                    if(state==XR_SESSION_STATE_READY) { XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO}; begin.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; XR(xrBeginSession(session,&begin)); running=true; prediction.reset(); history.clear(); hasVideo=false; }
                    if(state==XR_SESSION_STATE_STOPPING) { XR(xrEndSession(session)); running=false; }
                    if(state==XR_SESSION_STATE_EXITING||state==XR_SESSION_STATE_LOSS_PENDING) stopping=true;
                } else if(event.type==XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) stopping=true;
                else if(event.type==XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) { history.clear();hasVideo=false;prediction.reset(); }
                event={XR_TYPE_EVENT_DATA_BUFFER};
            }
            if(!running || stopping) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
            XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO}; XrFrameState frame{XR_TYPE_FRAME_STATE}; XR(xrWaitFrame(session,&wait,&frame)); XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO}; XR(xrBeginFrame(session,&begin));
            if(!refreshReported && frame.predictedDisplayPeriod>0) {
                float rate=1e9f/static_cast<float>(frame.predictedDisplayPeriod);
                if(rate>=30 && rate<=240) {
                    jclass cls=env->GetObjectClass(activity);
                    env->CallVoidMethod(activity,env->GetMethodID(cls,"reportRefreshRate","(F)V"),rate);
                    env->DeleteLocalRef(cls);
                    refreshReported=true;
                }
            }
            Snapshot snapshot{}; snapshot.displayTime=frame.predictedDisplayTime; snapshot.time=snapshot.displayTime+prediction.lead; for(auto& view:snapshot.views) view.type=XR_TYPE_VIEW;
            XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO}; locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; locate.displayTime=snapshot.time; locate.space=space;
            XrViewState viewState{XR_TYPE_VIEW_STATE}; uint32_t count; XR(xrLocateViews(session,&locate,&viewState,2,&count,snapshot.views.data()));
            bool validViews=count==2 && (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
            if(validViews) { tracking(snapshot); history.push_back(snapshot); while(history.size()>144) history.pop_front(); }
            jlong videoTime=env->CallLongMethod(activity,update,transformArray);
            if(videoTime>0) { env->GetFloatArrayRegion(transformArray,0,16,transform.data()); hasVideo=false; for(auto& entry:history) if(entry.time/1000==videoTime/1000) { rendered=entry;hasVideo=true;prediction.observe(frame.predictedDisplayTime,entry.displayTime);break; } }
            if(frame.predictedDisplayTime-predictionLogTime>=5000000000LL) {
                __android_log_print(ANDROID_LOG_INFO,"VR4Mac","Pose prediction lead %.1f ms; video age %.1f ms",prediction.lead/1e6,hasVideo?(frame.predictedDisplayTime-rendered.displayTime)/1e6:-1.0);
                predictionLogTime=frame.predictedDisplayTime;
            }
            if(hasVideo && frame.predictedDisplayTime-rendered.displayTime>1000000000LL) hasVideo=false;
            std::array<XrCompositionLayerProjectionView,2> projectionViews{};
            bool submit=validViews && frame.shouldRender;
            if(submit) for(int eye=0;eye<2;eye++) {
                uint32_t index; XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO}; XR(xrAcquireSwapchainImage(chains[eye],&acquire,&index)); XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout=XR_INFINITE_DURATION; XR(xrWaitSwapchainImage(chains[eye],&wi));
                glBindFramebuffer(GL_FRAMEBUFFER,fbo); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,images[eye][index].image,0); glViewport(0,0,sizes[eye].recommendedImageRectWidth,sizes[eye].recommendedImageRectHeight); glClearColor(0.02f,0.03f,0.06f,1); glClear(GL_COLOR_BUFFER_BIT);
                if(hasVideo) { glUseProgram(program);glBindVertexArray(vao);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_EXTERNAL_OES,texture);glUniform1i(glGetUniformLocation(program,"video"),0);glUniform1f(glGetUniformLocation(program,"eye"),(float)eye);glUniformMatrix4fv(glGetUniformLocation(program,"transform"),1,GL_FALSE,transform.data());glDrawArrays(GL_TRIANGLES,0,3); }
                glFlush(); XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; XR(xrReleaseSwapchainImage(chains[eye],&releaseInfo));
                auto& pv=projectionViews[eye];pv.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;pv.pose=hasVideo?rendered.views[eye].pose:snapshot.views[eye].pose;pv.fov=hasVideo?rendered.views[eye].fov:snapshot.views[eye].fov;pv.subImage.swapchain=chains[eye];pv.subImage.imageRect.extent={(int32_t)sizes[eye].recommendedImageRectWidth,(int32_t)sizes[eye].recommendedImageRectHeight};
            }
            XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};layer.space=space;layer.viewCount=2;layer.views=projectionViews.data(); const XrCompositionLayerBaseHeader* layers[]={reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=frame.predictedDisplayTime;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;end.layerCount=submit?1:0;end.layers=submit?layers:nullptr; XR(xrEndFrame(session,&end));
        }
    }
    void cleanup() {
        if(release) env->CallVoidMethod(activity,release);
        if(transformArray) env->DeleteLocalRef(transformArray);
        for(auto s:chains) if(s) xrDestroySwapchain(s);
        for(auto t:trackers) if(t&&destroyTracker) destroyTracker(t);
        for(auto s:aimSpace) if(s) xrDestroySpace(s);for(auto s:gripSpace) if(s) xrDestroySpace(s);
        if(headSpace) xrDestroySpace(headSpace);if(space) xrDestroySpace(space);if(session) xrDestroySession(session);if(actionSet) xrDestroyActionSet(actionSet);if(instance) xrDestroyInstance(instance);
        if(display!=EGL_NO_DISPLAY) { glDeleteTextures(1,&texture);glDeleteProgram(program);glDeleteFramebuffers(1,&fbo);glDeleteVertexArrays(1,&vao);eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);if(context!=EGL_NO_CONTEXT)eglDestroyContext(display,context);if(eglSurface!=EGL_NO_SURFACE)eglDestroySurface(display,eglSurface);eglTerminate(display); }
    }
};
extern "C" JNIEXPORT void JNICALL Java_com_vr4mac_client_MainActivity_runXR(JNIEnv* env,jobject activity) {
    stopping=false; Client c{};c.env=env;c.activity=activity;
    try { c.init(); c.loop(); } catch(const std::exception& e) { __android_log_print(ANDROID_LOG_ERROR,"VR4Mac","%s",e.what()); jclass cls=env->GetObjectClass(activity);auto report=env->GetMethodID(cls,"reportError","(Ljava/lang/String;)V");jstring msg=env->NewStringUTF(e.what());env->CallVoidMethod(activity,report,msg);env->DeleteLocalRef(msg); }
    c.cleanup();
}
extern "C" JNIEXPORT void JNICALL Java_com_vr4mac_client_MainActivity_stopXR(JNIEnv*,jobject) { stopping=true; }
extern "C" JNIEXPORT void JNICALL Java_com_vr4mac_client_MainActivity_haptic(JNIEnv*,jobject,jint hand,jfloat amplitude,jfloat duration,jfloat frequency) { std::lock_guard<std::mutex> lock(hapticMutex); if(haptics.size()<32) haptics.push_back({hand,amplitude,duration,frequency}); }
