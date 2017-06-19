//
// Created by Lukas Kollmer on 17.04.17.
//

#include <node.h>
#include <string>
#include <iostream>
#include <set>
#include <map>
#include "Proxy.h"
#include "objc_call.h"

#include "Invocation.h"
#include "utils.h"

extern "C" {
#include <objc/objc-exception.h>
}

#define v8String(str) v8::String::NewFromUtf8(isolate, str)


using std::vector;
using std::string;



/**
 * This is a small helper class to wrap an `id` object.
 *
 * Why? Because v8 expects 2-byte aligned objects and we can't control whether `id`s we get from the objc runtime are aligned
 * But we can store the misaligned pointer in an aligned pointer to the wrapper
 */
class AlignedObjectWrapper {
public:
    AlignedObjectWrapper(id object) : object(object) {}
    id object;
};

#define HANDLE_RETURN_TYPE(type) \
    type retval; \
    invocation.GetReturnValue(&retval); \
    args.GetReturnValue().Set(retval); \
    return; \

#define HANDLE_RETURN_TYPE_CAST(type, castType) \
    type retval; \
    invocation.GetReturnValue(&retval); \
    args.GetReturnValue().Set((castType)retval); \
    return; \

#define ARGTYPE_NOT_SUPPORTED(type) \
    char *excMessage; \
    asprintf(&excMessage, "Error setting argument: Type '%s' not yet supported. Sorry.", type); \
    isolate->ThrowException(v8::Exception::Error(v8String(excMessage))); \
    free(excMessage); \
    return; \



using namespace v8;

namespace ObjC {

    Persistent<Function> Proxy::constructor;

    std::map<string, vector<string>> cachedClassMethods;
    std::map<string, vector<string>> cachedInstanceMethods;

    Proxy::Proxy(enum Type type, id obj) : type_(type), obj_(obj) {}
    Proxy::~Proxy() {}

    void Proxy::Init(Local<Object> exports) {
        Isolate *isolate = exports->GetIsolate();
        HandleScope scope(isolate);

        Local<FunctionTemplate> tpl = FunctionTemplate::New(isolate, New);
        tpl->SetClassName(v8String("Proxy"));
        tpl->InstanceTemplate()->SetInternalFieldCount(1);

        NODE_SET_PROTOTYPE_METHOD(tpl, "call", Call);
        NODE_SET_PROTOTYPE_METHOD(tpl, "description", Description);
        NODE_SET_PROTOTYPE_METHOD(tpl, "isNil", IsNil);

        NODE_SET_PROTOTYPE_METHOD(tpl, "methods", Methods);
        //NODE_SET_PROTOTYPE_METHOD(tpl, "hasMethod", HasMethod);

        NODE_SET_PROTOTYPE_METHOD(tpl, "type", Type);
        NODE_SET_PROTOTYPE_METHOD(tpl, "returnTypeOfMethod", ReturnTypeOfMethod);


        constructor.Reset(isolate, tpl->GetFunction());
        exports->Set(v8String("Proxy"), tpl->GetFunction());
    }


    void Proxy::New(const FunctionCallbackInfo<Value>& args) {
        Isolate *isolate = args.GetIsolate();
        HandleScope scope(isolate);

        id object;

        enum Type type = static_cast<enum Type>(args[0]->Int32Value());
        switch (type) {
            case Type::klass: {
                auto classname = ValueToChar(isolate, args[1]);
                object = (id)objc_getClass(classname);
                if (object == NULL) {
                    char *excMessage;
                    asprintf(&excMessage, "Error: Class with name '%s' doesn't exist", classname);
                    isolate->ThrowException(v8::Exception::Error(v8String( excMessage)));
                    free(excMessage);
                    return;
                }
                break;
            }
            case Type::instance: {
                auto wrapper = (AlignedObjectWrapper* )args[1]->ToObject()->GetAlignedPointerFromInternalField(0);
                object = wrapper->object;
                break;
            }
        }

        Proxy *proxy = new Proxy(type, object);
        proxy->Wrap(args.This());

        args.GetReturnValue().Set(args.This());


    }

    void Proxy::Type(const FunctionCallbackInfo<Value> &args) {
        Isolate *isolate = args.GetIsolate();
        HandleScope scope(isolate);

        Proxy *obj = ObjectWrap::Unwrap<Proxy>(args.This());

        int type = static_cast<int>(obj->type_);

        args.GetReturnValue().Set(type);
    }

    void Proxy::Description(const FunctionCallbackInfo<Value> &args) {
        Isolate *isolate = args.GetIsolate();
        HandleScope scope(isolate);

        Proxy *object = ObjectWrap::Unwrap<Proxy>(args.This());

        id description = objc_call(id, object->obj_, "description");
        char *desc = objc_call(char*, description, "UTF8String");

        args.GetReturnValue().Set(v8String(desc));

    }

    void Proxy::IsNil(const FunctionCallbackInfo<Value> &args) {
        Isolate *isolate = args.GetIsolate();
        HandleScope scope(isolate);

        Proxy *obj = ObjectWrap::Unwrap<Proxy>(args.This());
        auto isNil = obj->obj_ == nil;

        args.GetReturnValue().Set(isNil);
    }

    void Proxy::Methods(const FunctionCallbackInfo<Value> &args) {
        Isolate *isolate = args.GetIsolate();
        HandleScope scope(isolate);

        const char *methodType = ValueToChar(isolate, args[0]);

        Proxy *obj = ObjectWrap::Unwrap<Proxy>(args.This());

        // TODO: Add an option to load _only_ the methods of the class, not methods it inherited from its superclasses
        auto getMethodsOfClass = [](Class cls) -> vector<string> {
            std::set<std::string>methods;

            do {
                uint count = 0;
                Method* methodList = class_copyMethodList(cls, &count);

                for (int i = 0; i < (int)count; ++i) {
                    Method m = methodList[i];
                    methods.insert(sel_getName(method_getName(m)));
                }
                free(methodList);
            } while ((cls = class_getSuperclass(cls)));

            return vector<string>(methods.begin(), methods.end());
        };


        Class classOfObject = ([&](id object) -> Class {
            switch (obj->type_) {
                case Type::klass: return (Class)object;
                case Type::instance: return objc_call(Class, object, "classForCoder");
            }
        })(obj->obj_);


        Class cls = EQUAL(methodType, "class") ? object_getClass((id)classOfObject) : classOfObject;
        auto classname = string(class_getName(cls));

        vector<string> methods;
        auto cachedMethods = (EQUAL(methodType, "class") ? cachedClassMethods : cachedInstanceMethods)[classname];

        if (cachedMethods.size() != 0) {
            methods = cachedMethods;
        } else {
            methods = getMethodsOfClass(cls);
            (EQUAL(methodType, "class") ? cachedClassMethods : cachedInstanceMethods)[classname] = methods;
        }

        Local<Array> javaScriptMethodList = Array::New(isolate, (int) methods.size());

        for (int i = 0; i < (int)methods.size(); ++i) {
            javaScriptMethodList->Set((uint32_t) i, v8String(methods[i].c_str()));
        }

        args.GetReturnValue().Set(javaScriptMethodList);
    }

    void Proxy::Call(const FunctionCallbackInfo<Value>& args) {
        Isolate *isolate = args.GetIsolate();
        HandleScope scope(isolate);

        Local<ObjectTemplate> TemplateObject = ObjectTemplate::New(isolate);
        TemplateObject->SetInternalFieldCount(1);

        Local<String> __ptr_key = v8String("__ptr");
        Local<String> __ref_key = v8String("ref");

        //auto isKindOfClass = [](id object, const char *classname) -> bool {
        //   return objc_call(bool, object, "isKindOfClass:", objc_getClass(classname));
        //};


        // Wrap an `id` in a `AlignedObjectWrapper` in a `ObjC::Proxy` in a `Local<Object>` that can be returned by v8
        // TODO v8::External might work as well

        // Returns:
        // Local<Value>
        // └── AlignedObjectWrapper
        //     └── ObjC::Proxy
        //         └── id
        // (sorry)
        auto CreateNewObjCWrapperFrom = [&](id obj) -> Local<Object> {
            // aligning-code adapted from http://stackoverflow.com/a/6320314/2513803
            void * p = new AlignedObjectWrapper(obj);

            std::size_t const size = sizeof(p);
            std::size_t space = size;
            void* aligned = std::align(2, sizeof(p), p, space);
            if(aligned == nullptr) {
                // failed to align
                isolate->ThrowException(Exception::Error(v8String("Internal Error: Unable to align pointer")));
                return Undefined(isolate)->ToObject();
            } else {
                Local<Object> object = TemplateObject->NewInstance();
                object->SetAlignedPointerInInternalField(0, aligned);
                // TODO ^^ This sometimes crashes w/ a "pointer not aligned" error message (seems to happen at random, should try to fix eventually)

                const unsigned argc = 2;
                Local<Value> argv[argc] = {Number::New(isolate, 1), object};
                Local<Function> cons = Local<Function>::New(isolate, constructor);
                Local<Context> context = isolate->GetCurrentContext();
                Local<Object> instance = cons->NewInstance(context, argc, argv).ToLocalChecked();

                return instance;
            }
        };

        Proxy *obj = ObjectWrap::Unwrap<Proxy>(args.This());

        SEL sel = sel_getUid(ValueToChar(isolate, args[0]));

        Method method;

        switch (obj->type_) {
            case Type::klass: method = class_getClassMethod((Class)obj->obj_, sel); break;
            case Type::instance: method = class_getInstanceMethod(object_getClass(obj->obj_), sel); break;
        }

        //printf("%s - %s\n", sel_getName(sel), method_getTypeEncoding(method));


        auto invocation = ObjC::Invocation(obj->obj_, sel);
        invocation.SetTarget(obj->obj_);
        invocation.SetSelector(sel);

        // The argument indexes of all inout arguments (^@)
        std::set<int> inoutArgs;

        // Why not use auto? -> http://stackoverflow.com/a/14532044/2513803
        std::function<id (Local<Value>, const char*)> convertJavaScriptObjectToCorrespondingObjCType = [&](Local<Value> arg, const char* expectedType) -> id {

            if (arg->IsObject() && !arg->IsArray()) { // Arrays are objects too
                Local<Object> wrappedObject;

                if (EQUAL(expectedType, "@")) {
                    // args[i] is the JS Proxy type, we have to fetch the actual ObjC::Proxy wrapper via __ptr
                    wrappedObject = arg->ToObject()->Get(__ptr_key)->ToObject();
                } else if (EQUAL(expectedType, "^@")) {
                    auto wrappedProxy = arg->ToObject()->Get(__ref_key);
                    auto proxyIsNull = wrappedProxy->IsUndefined() || wrappedProxy->IsNull();

                    if (proxyIsNull) {
                        id _nullptr = (id)malloc(sizeof(id));
                        return _nullptr;
                    } else {
                        wrappedObject = arg->ToObject()->Get(__ref_key)->ToObject()->Get(__ptr_key)->ToObject();
                    }
                }

                Proxy *passedProxy = ObjectWrap::Unwrap<Proxy>(wrappedObject);
                if (passedProxy != nullptr) {
                    id argument = passedProxy->obj_;
                    return argument;
                } else {
                    // TODO set nil as arg
                }
                // 2. if no wrapped object was passed, but a native type (string, number, bool), convert that
            } else if (arg->IsString()) {
                const char* stringValue = ValueToChar(isolate, arg);

                id NSString = (id)objc_getClass("NSString");
                id string = objc_call(id, NSString, "stringWithUTF8String:", stringValue);

                return string;
            } else if (arg->IsNumber()) {
                double numberValue = arg->ToNumber()->Value();

                id NSNumber = (id)objc_getClass("NSNumber");
                id number = objc_call(id, NSNumber, "numberWithDouble:", numberValue);

                return number;
            } else if (arg->IsBoolean()) {
                bool boolValue = arg->ToBoolean()->Value();

                id NSNumber = (id)objc_getClass("NSNumber");
                id number = objc_call(id, NSNumber, "numberWithBool:", boolValue);

                return number;
            } else if (arg->IsArray()) {  // TODO Convert array/dict as well?
                Local<Array> argArray = Local<Array>::Cast(arg);

                id NSMutableArray = (id)objc_getClass("NSMutableArray");
                id objcArray = objc_call(id, NSMutableArray, "array");

                for (int j = 0; j < (int)argArray->Length(); ++j) {
                    id arrayObject = convertJavaScriptObjectToCorrespondingObjCType(argArray->Get((uint32_t) j), "@");
                    objc_call_noreturn(void, objcArray, "addObject:", arrayObject);
                }

                return objcArray;
            }

            return nil;

        };


        for (int i = 1; i < args.Length(); ++i) {
            int objcArgumentIndex = i + 1; // +1 bc we already start at 1 (0 is the method name, added by the objc js module)

            auto expectedType = method_copyArgumentType(method, (unsigned int) objcArgumentIndex);
            Local<Value> arg = args[i];

            if (arg->IsNull() || arg->IsUndefined()) {
                void* nilArgument = nullptr;
                invocation.SetArgumentAtIndex(&nilArgument, objcArgumentIndex);
                continue;
            }

            if (EQUAL(expectedType, "^@")) { // inout object
                inoutArgs.insert(objcArgumentIndex);
            }

            if (EQUAL(expectedType, "@") || EQUAL(expectedType, "^@")) {
                id object = convertJavaScriptObjectToCorrespondingObjCType(arg, expectedType);
                invocation.SetArgumentAtIndex(&object, objcArgumentIndex);

            } else if (EQUAL(expectedType, "#")) { // Class
                // This will either take a proxy around a `Class` object or a string and convert that to the expected `Class` object
                if (arg->IsString()) {
                    const char *classname = ValueToChar(isolate, arg);
                    Class cls = objc_getClass(classname);
                    invocation.SetArgumentAtIndex(&cls, objcArgumentIndex);
                } else if (arg->IsObject()) {
                    Local<Object> wrappedObject = arg->ToObject()->Get(v8String("__ptr"))->ToObject();

                    Proxy *passedClassProxy = ObjectWrap::Unwrap<Proxy>(wrappedObject);
                    if (passedClassProxy != nullptr) {
                        if (passedClassProxy->type_ == Type::klass) {
                            Class cls = (Class) passedClassProxy->obj_;
                            invocation.SetArgumentAtIndex(&cls, objcArgumentIndex);
                        } else {
                            // TODO ???
                        }
                    } else {
                        // TODO pass nil?
                    }
                }
            } else if (EQUAL(expectedType, "@?")) { // Block
                ARGTYPE_NOT_SUPPORTED("Block");
                /*
                printf("expects a block at #%i\n", objcArgumentIndex);

                auto classname = ValueToChar(isolate, arg->ToObject()->GetConstructorName());

                if (EQUAL(classname, "Block")) {
                    //auto fn = arg->ToObject()->Get(v8String("fn")).As<Function>();
                }

                auto fn = Local<Function>::Cast(arg);

                printf("callable: %i\n", fn->IsCallable());

                Local<Value> argss[2];
                argss[0] = Number::New(isolate, 1);
                argss[1] = Number::New(isolate, 2);

                auto res = fn->Call(Undefined(isolate), 2, argss);

                printf("val: %lf\n", res->ToNumber()->Value());

                auto ext = arg.As<External>();
                printf("ext->Value(): %p\n", ext->Value());


                auto ext2 = Local<External>::New(isolate, External::New(isolate, NULL));
                printf("ext2->Value(): %p\n", ext2->Value());



                //printf("ext: %p\n", ext);
                //printf("ext value: %p\n", ext->Value());

                //int (*fn) (int, int) = (int (*) (int, int)) ext->Value();

                //printf("fn is nil: %i\n", fn == nullptr);

                //auto fn = object->GetAlignedPointerFromInternalField(0);

                //printf("fn: %p\n", fn);*/



            } else if (EQUAL(expectedType, "c")) { // char
                ARGTYPE_NOT_SUPPORTED("char");
            } else if (EQUAL(expectedType, "i")) { // int
                int argument = arg->ToNumber()->ToInt32()->Value();
                invocation.SetArgumentAtIndex(&argument, objcArgumentIndex);
            } else if (EQUAL(expectedType, "s")) { // short
                ARGTYPE_NOT_SUPPORTED("short");
            } else if (EQUAL(expectedType, "q")) { // long long
                long long argument = (long long) arg->ToNumber()->Value();
                invocation.SetArgumentAtIndex(&argument, objcArgumentIndex);
            } else if (EQUAL(expectedType, "C")) { // unsigned char
                ARGTYPE_NOT_SUPPORTED("unsigned char");
            } else if (EQUAL(expectedType, "I")) { // unsigned int
                unsigned int argument = (unsigned int) arg->ToNumber()->Int32Value();
                invocation.SetArgumentAtIndex(&argument, objcArgumentIndex);
            } else if (EQUAL(expectedType, "S")) { // unsigned short
                ARGTYPE_NOT_SUPPORTED("unsigned short");
            } else if (EQUAL(expectedType, "L")) { // unsigned long
                unsigned long argument = (unsigned long) arg->ToNumber()->Int32Value();
                invocation.SetArgumentAtIndex(&argument, objcArgumentIndex);
            } else if (EQUAL(expectedType, "Q")) { // unsigned long long
                unsigned long long argument = (unsigned long long) arg->ToNumber()->Int32Value();
                invocation.SetArgumentAtIndex(&argument, objcArgumentIndex);
            } else if (EQUAL(expectedType, "f")) { // float
                float argument = (float) arg->ToNumber()->Value();
                invocation.SetArgumentAtIndex(&argument, objcArgumentIndex);
            } else if (EQUAL(expectedType, "d")) { // double
                double argument = arg->ToNumber()->Value();
                invocation.SetArgumentAtIndex(&argument, objcArgumentIndex);
            } else if (EQUAL(expectedType, "B")) { // bool
                bool argument = arg->ToBoolean()->Value();
                invocation.SetArgumentAtIndex(&argument, objcArgumentIndex);
            } else if (EQUAL(expectedType, "v")) { // void
                ARGTYPE_NOT_SUPPORTED("void");
            } else if (EQUAL(expectedType, "*") || EQUAL(expectedType, "r*")) { // char*, const char*
                ARGTYPE_NOT_SUPPORTED("char*");
            } else if (EQUAL(expectedType, ":")) { // SEL
                // selectors can be passed as strings.
                SEL _sel = sel_getUid(ValueToChar(isolate, arg));
                invocation.SetArgumentAtIndex(&_sel, objcArgumentIndex);
            } else if (EQUAL(expectedType, "^v") || EQUAL(expectedType, "r^v")) { // void*, const void*
                ARGTYPE_NOT_SUPPORTED("void*");
            }
        }

        //
        // Setup Error Handling
        //

        // Why are we using `objc_setExceptionPreprocessor`?
        // 1. `objc_setUncaughtExceptionHandler` always terminates the process, meaning that, even if an exception is caught in JS land,
        //    node will still crash
        // 2. To prevent v8 (and node) from crashing, we NEED to call `invocation.Invoke` in a `try {}` block.
        //    (which means that `objc_setUncaughtExceptionHandler` wouldn't get called since the exception isn't uncaught anymore)
        // 3. `objc_addExceptionHandler` doesn't seem to work at all

        // The way this is currently implemented is that we abuse the exception preprocessor to do the actual exception handling
        // This works because the preprocessor is always called, even if the exception is caught.
        // In the preprocessor, we simply throw a regular JS exception which is caught in JS land and then rethrown to get a nicer stacktrace
        // TODO: Maybe provide an objc stacktrace as well?

        auto objc_exception_preprocessor = + [] (id exc) -> id {
            Isolate *isolate = Isolate::GetCurrent();

            auto exc_name_obj = objc_call(id, exc, "name");
            auto exc_name_str = objc_call(const char*, exc_name_obj, "UTF8String");

            auto exc_reason_obj = objc_call(id, exc, "reason");
            auto exc_reason_str = objc_call(const char*, exc_reason_obj, "UTF8String");

            std::string exceptionMessage(exc_name_str);
            exceptionMessage.append(" ");
            exceptionMessage.append(exc_reason_str);

            auto errorMessage = v8::String::NewFromUtf8(isolate, exceptionMessage.c_str());

            isolate->ThrowException(Exception::Error(errorMessage));

            return exc;
        };

        objc_setExceptionPreprocessor(objc_exception_preprocessor);


        //
        // Invoke
        //

        try {
            invocation.Invoke();
        } catch (...) {
            args.GetReturnValue().Set(Undefined(isolate));
            return;
        }



        //
        // Handle inout args
        //

        for (auto &&objcArgumentIndex : inoutArgs) {
            int javaScriptArgumentIndex = objcArgumentIndex - 1; // 0 is the method name

            id *arg = (id *)malloc(sizeof(id));
            invocation.GetArgumentAtIndex(&arg, objcArgumentIndex);
            id unwrappedArg = *arg;

            if (unwrappedArg != nil) {
                auto wrappedValue = CreateNewObjCWrapperFrom(unwrappedArg);
                args[javaScriptArgumentIndex]->ToObject()->Set(__ref_key, wrappedValue);
            } else {
                // TODO test this
                args[javaScriptArgumentIndex]->ToObject()->Set(__ref_key, Undefined(isolate)); // TODO set Null instead
            }
        }


        //
        // Handle return value
        //

        const char *returnType = method_copyReturnType(method);
        //printf("%s - %s\n", returnType, sel_getName(sel));

        if (EQUAL(returnType, "@")) {
            id retval = (id) malloc(sizeof(id));
            invocation.GetReturnValue(&retval);
            args.GetReturnValue().Set(CreateNewObjCWrapperFrom(retval));

            return;


            /**
             * TODO: Implement this properly. Maybe define a package-wide config variable to allow the user to choose whether the objc module should always return wrapped objc objects
             * date: 2017-05-16 09:15
             */
            //if (isKindOfClass(retval, "NSString")) {
            //    char *charValue = objc_call(char*, retval, "UTF8String");
            //    args.GetReturnValue().Set(String::NewFromUtf8(isolate, charValue));
            //    return;
            //}

            //if (isKindOfClass(retval, "NSNumber")) {
            //    double value = objc_call(double, retval, "doubleValue");
            //    args.GetReturnValue().Set(value);
            //    return;
            //}
        } else if (EQUAL(returnType, "c")) { // char
            // Fun Fact: ObjC BOOLs are encoded as char https://developer.apple.com/reference/objectivec/bool?language=objc
            HANDLE_RETURN_TYPE(char);
        } else if (EQUAL(returnType, "i")) { // int
            HANDLE_RETURN_TYPE(int);
        } else if (EQUAL(returnType, "s")) { // short
            HANDLE_RETURN_TYPE(short);
        } else if (EQUAL(returnType, "q")) { // long long
            HANDLE_RETURN_TYPE_CAST(long long, int32_t);
        } else if (EQUAL(returnType, "C")) { // unsigned char
            HANDLE_RETURN_TYPE(unsigned char);
        } else if (EQUAL(returnType, "I")) { // unsigned int
            HANDLE_RETURN_TYPE(unsigned int);
        } else if (EQUAL(returnType, "S")) { // unsigned short
            HANDLE_RETURN_TYPE(unsigned short);
        } else if (EQUAL(returnType, "L")) { // unsigned long
            HANDLE_RETURN_TYPE_CAST(unsigned long, int32_t);
        } else if (EQUAL(returnType, "Q")) { // unsigned long long
            HANDLE_RETURN_TYPE_CAST(unsigned long long, int32_t);
        } else if (EQUAL(returnType, "f")) { // float
            HANDLE_RETURN_TYPE(float);
        } else if (EQUAL(returnType, "d")) { // double
            HANDLE_RETURN_TYPE(double);
        } else if (EQUAL(returnType, "B")) { // bool
            HANDLE_RETURN_TYPE(BOOL);
        } else if (EQUAL(returnType, "v")) { // void
            args.GetReturnValue().Set(Undefined(isolate));
        } else if (EQUAL(returnType, "*") || EQUAL(returnType, "r*")) { // char*, const char*
            char* retval;
            invocation.GetReturnValue(&retval);
            Local<Value> string = v8String(retval);
            args.GetReturnValue().Set(string);
            return;
        } else if (EQUAL(returnType, "#")) { // Class
            // TODO
            args.GetReturnValue().Set(Undefined(isolate));
        } else if (EQUAL(returnType, ":")) { // SEL
            // TODO
            args.GetReturnValue().Set(Undefined(isolate));
        }

        return;
    }

    void Proxy::ReturnTypeOfMethod(const FunctionCallbackInfo<Value> &args) {
        Isolate *isolate = args.GetIsolate();
        HandleScope scope(isolate);

        Proxy *obj = ObjectWrap::Unwrap<Proxy>(args.This());
        SEL sel = sel_getUid(ValueToChar(isolate, args[0]));

        Method method;

        switch (obj->type_) {
            case Type::klass: method = class_getClassMethod((Class)obj->obj_, sel); break;
            case Type::instance: method = class_getInstanceMethod(object_getClass(obj->obj_), sel); break;
        }

        const char *returnType = method_copyReturnType(method);

        args.GetReturnValue().Set(v8String(returnType));
    }
}
