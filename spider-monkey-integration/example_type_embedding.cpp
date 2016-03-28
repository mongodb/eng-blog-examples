// Imagine for a moment that you've read and implemented
// something from the SpiderMonkey hello world
// (https://developer.mozilla.org/en-US/docs/Mozilla/Projects/SpiderMonkey/How_to_embed_the_JavaScript_engine),
// and that you've come to terms with the way they track
// garbage collected objects
// (https://developer.mozilla.org/en-US/docs/Mozilla/Projects/SpiderMonkey/GC_Rooting_Guide).
//
// You've taken care of initializing the runtime, created a
// context and global scope object and wrapped it all up in
// a Read Eval Print Loop.  Now your application can take
// JavaScript in and process its output in some rudimentary
// way (perhaps printing to standard out).  At this point,
// you realize that you'd like one of your callbacks to
// return an integer value, specifically one that requires
// 64 bits to fully represent.
// 
// You could go the route of returning a string, except that
// all of the encoding and decoding will be quite expensive.
//
// You could use a double, except that values over 2^53
// will experience rounding due to a lack of resolution
// (standard IEEE 754 doubles only hold 52 bits of
// mantissa).
//
// But all of those seem a little too error prone and not
// quite as flexible as you'd like, so you opt for a custom
// type. It will encapsulate a heap allocated C++ int64_t
// and will expose a few methods as accessors.  You'd like
// your shim to create objects of this type, check if an
// object you are handed is one of them and ensure that all
// of this is resilient to adversarial use (so avoid
// crashes, use after frees, etc).

// Without any scaffolding, you might start off with
// something like this: (Note that we do something very
// similar to represent 64 bit signed integers faithfully in
// the mongo shell)

// The actual type you're adapting.  For now we'll make it a
// simple wrapper around an int64_t
struct MyType {
    int64_t val;
};

// Now we'll wrap up all of the various handles we'll need
// into SpiderMonkey to shim our type into the environment.

// Any type we want to adapt to the SpiderMonkey environment
// will require some code that closely resembles the
// boilerplate below.

class AdaptedMyType {
public:
    // Assume that the context has been initialized and that
    // the global object has already been created.
    //
    // The context object is a handle to the javascript
    // execution environment, with it's own callstack, heap,
    // etc.
    //
    // The global object is the top level scope where global
    // variables go.  I.e. if you execute "x = 10;" the
    // global object will then hold { x : 10 }
    AdaptedMyType(JSContext* ctx, JS::HandleObject global)
        : _context(ctx),

          // A jsclass can be thought of as the vtable
          // behind a type.  We attach lifecycle methods to
          // it which, when present, modify behavior.  Most
          // of these fields are empty in this example
          // because we don’t want to specialize their
          // behavior.  As an example, providing an
          // enumerate function would specialize field
          // lookup on our type.
          _jsclass{
              "MyType",
              JSCLASS_HAS_PRIVATE,
              nullptr,  // addProperty
              nullptr,  // delProperty
              nullptr,  // getProperty
              nullptr,  // setProperty
              nullptr,  // enumerate
              nullptr,  // resolve
              nullptr,  // convert
              AdaptedMyType::finalize,
              nullptr,  // call
              nullptr,  // hasInstance
              AdaptedMyType::construct,
          },

          // The JavaScript prototype object for the type
          // we’re adapting will hold a value returned from
          // JS_InitClass.  That helper is provided by
          // SpiderMonkey and wraps up binding of methods,
          // constructors, etc.
          _proto(
              _context,
              JS_InitClass(
                  _context,
                  global,         // global scope to install into
                  JS::NullPtr(),  // parent of the prototype
                  &_jsclass,
                  construct,
                  0,        // hint # of args to constructor
                  nullptr,  // instance property spec, i.e.
                            // attributes
                  methods,  // instance function spec, i.e.
                            // methods
                  nullptr,  // static property spec
                  nullptr   // static function spec
                  )) {}

    // We'll use this function to make new objects of our
    // desired type from C++
    void newObject(JS::MutableHandleObject out) {
        out.set(JS_NewObject(_context, &_jsclass, _proto));
    }

    // Check if an object is of this type
    bool instanceOf(JS::HandleObject object) {
        return JS_InstanceOf(
            _context, object, &_jsclass, nullptr);
    }

private:
    // Every JSContext will hold an instance of our adapter
    // type in which the bookkeeping specific to that
    // context is done.  Thus we provide a function here to
    // get the specific adapter needed for the given context
    // object.  The implementation is elided here for
    // brevity.
    static AdaptedMyType& fromContext(JSContext* cx);

    // We specialize finalization of our adapted type by
    // fetching our private C++ implementation out of it and
    // calling delete.  This is called when the JS object is
    // GC’d.
    static void finalize(JSFreeOp* fop, JSObject* obj) {
        // JS_Get/Set_Private() provides access to a special
        // void* attached to a given JSObject. We use it to
        // store a heap allocated MyType that holds the data
        // we care about.
        auto ptr = static_cast<MyType*>(JS_GetPrivate(obj));
        if (ptr)
            delete ptr;
    }

    // Our constructor is of the form MyType("12345").  That
    // allows us to bind integers that can't be represented
    // by a double.
    static bool construct(JSContext* cx,
                          unsigned argc,
                          JS::Value* vp) {
        auto args = JS::CallArgsFromVp(argc, vp);

        JS::RootedString str(cx, args.get(0).toString());

        // SpiderMonkey strings are utf16 internally,
        // JSAutoByteString manages converting to utf8
        // safely
        JSAutoByteString bstr(cx, str);
        auto val = std::atoll(bstr.ptr());

        auto myType = std::make_unique<MyType>(MyType{val});

        JS::RootedObject out(cx);
        fromContext(cx).newObject(&out);
        JS_SetPrivate(out, myType.release());

        args.rval().setObjectOrNull(out);

        return true;
    }

    static bool toNumber(JSContext* cx,
                         unsigned argc,
                         JS::Value* vp) {
        auto args = JS::CallArgsFromVp(argc, vp);

        auto ptr = static_cast<MyType*>(
            JS_GetPrivate(args.thisv().toObjectOrNull()));
        args.rval().setNumber(
            static_cast<double>(ptr->val));

        return true;
    }

    static bool toString(JSContext* cx,
                         unsigned argc,
                         JS::Value* vp) {
        auto args = JS::CallArgsFromVp(argc, vp);

        auto ptr = static_cast<MyType*>(
            JS_GetPrivate(args.thisv().toObjectOrNull()));
        auto str = std::to_string(ptr->val);
        JS::RootedString rstr(
            cx, JS_NewStringCopyZ(cx, str.c_str()));
        args.rval().setString(rstr);

        return true;
    }

    static constexpr JSFunctionSpec methods[] = {
        JS_FS("toNumber", &toNumber, 0, 0),
        JS_FS("toString", &toString, 0, 0),
        JS_FS_END,
    };

    JSContext* _context;
    JSClass _jsclass;
    JS::PersistentRootedObject _proto;
};


// While this is enough to work, it's worth noting a number
// of things that we're not doing that make this an unsafe
// integration:
//
// 1. The vast majority of SpiderMonkey calls can fail.  All
//    of them need to have their error returns checked.
// 2. SpiderMonkey requires that callbacks not throw.  We
//    need to make sure that exceptions are trapped and that
//    callbacks return false when they are.
// 3. An adversarial user of our library can invoke the
//    methods we've created on our prototype (which only holds
//    a nullptr) or on completely unrelated types (where the
//    JS_GetPrivate call may read completely arbitrary data).
//    We need to constrain method invocation to objects of the
//    correct type.
//
// Let's see what that looks like for toNumber:

// Assume a
// lippincott(http://cppsecrets.blogspot.com/2013/12/using-lippincott-function-for.html)
// function that encodes a c++ exception as a JavaScript
// exception.
void cppToJSException(JSContext* cx);

static bool toNumber(JSContext* cx,
                     unsigned argc,
                     JS::Value* vp) {
    try {
        auto args = JS::CallArgsFromVp(argc, vp);

        if (!args.thisv().isObject()) {
            throw std::runtime_error(std::string(
                "MyType::toNumber can only be called on "
                "objects"));
        }

        JS::RootedObject obj(cx,
                             args.thisv().toObjectOrNull());

        if (!fromContext(cx).instanceOf(obj)) {
            throw std::runtime_error(std::string(
                "MyType::toNumber can only be called on "
                "objects of type MyType"));
        }

        if (fromContext(cx)._proto == obj) {
            throw std::runtime_error(std::string(
                "MyType::toNumber can't be called on the "
                "prototype"));
        }

        auto ptr = static_cast<MyType*>(
            JS_GetPrivate(args.thisv().toObjectOrNull()));
        args.rval().setNumber(
            static_cast<double>(ptr->val));

        return true;
    } catch (...) {
        cppToJSException(cx);
        return false;
    }
}

// Now repeat that kind of logic for all of the other
// callbacks.

// And after we've made our first integration robust, let's
// look at what we'll have to do for our second, third and
// 20th type.
// 1. There's a lot of boiler plate floating around, and
//    much of it is quite typo prone (the JSClass and
//    JS_InitClass invocations will be easy to screw up once we
//    start adding pointers).  For example, you might want to
//    provide a addProperty handler, but accidentally put it in
//    the delProperty slot.  The type system will not help you.
// 2. Small changes in functionality involve large changes
//    to our boilerplate.  As an example, if we'd like to make
//    a type without a globally visible constructor, we
//    actually won't be able to use JS_InitClass (not only does
//    it expose a global constructor, but deleting the exposed
//    constructor later will prevent prototype lookup due to an
//    optimization within JS_InitClass).

// What sort of tricks can we imagine doing to save
// ourselves that boilerplate?  We could attack it with
// manual codegen, but first lets see what C++ can natively
// give us.
//
// We'll need something with the correct signature for
// SpiderMonkey and we'll need unique function pointers per
// callback.  The obvious solution is drive template
// instantiations per callback, which we can make unique by
// making each callback a type.

template <typename T>
bool wrapFunction(JSContext* cx,
                  unsigned argc,
                  JS::Value* vp) {
    try {
        JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
        T::call(cx, args);
        return true;
    } catch (...) {
        cppToJSException(cx);
        return false;
    }
}

// Now our users just write their callbacks of the form:

struct Callback {
    static const char* name() {
        return "CallbackName";
    };
    static void call(JSContext* cx, JS::CallArgs args);
};

// And they can throw if they want, don't have to worry
// about early returns and always get their exceptions
// massaged.
//
// That's great for any free functions, or those that don't
// rely on opaque private pointers, but what about those
// additional method constraints?

// Let's imagine a function which takes an object and checks
// it against several types like AdaptedMyType.  It returns
// a tuple of bools where:
//
// 1. The given type is one of the Args types
// 2. The given type is the prototype of one of the given
// types.
template <typename T, typename... Args>
std::tuple<bool, bool> instanceOf(JSContext* cx,
                                  JS::HandleValue value);

// Now provide a generator for all constrained methods.
template <typename T, bool noProto, typename... Args>
bool wrapConstrainedMethod(JSContext* cx,
                           unsigned argc,
                           JS::Value* vp) {
    try {
        JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

        if (!args.thisv().isObject()) {
            throw std::runtime_error(
                std::string(T::name()) +
                " can only be called on objects");
        }

        bool correctType;
        bool isProto;

        std::tie(correctType, isProto) =
            instanceOf<Args...>(cx, args.thisv());

        if (!correctType) {
            throw std::runtime_error(
                std::string(T::name()) +
                " can only be called on objects of the "
                "correct type");
        }

        if (noProto && isProto) {
            throw std::runtime_error(
                std::string(T::name()) +
                " cannot be called on the prototype");
        }

        T::call(cx, args);
        return true;
    } catch (...) {
        cppToJSException(cx);
        return false;
    }
}

// That takes care of producing valid callbacks, with all of
// the necessary boiler plate.  But what about stamping out
// multiple whole types, rather than just callbacks for the
// type (a decimal floating point lets say)?  For that, we
// can turn to the same kind of policy dispatch we just used
// for wrapFunction, but with a more complicated shape.
//
// Policy based design is a powerful technique for providing
// compile time customization of functions and types.  This
// link (https://en.wikipedia.org/wiki/Policy-based_design)
// describes it in better detail if you’re interested.  The
// main point here is to enumerate all of the kinds of
// specialization we want to do for all of our custom types.
// We'll inherit our other type policies from this base
// policy to allow for easy type reflection by comparing
// member pointers.  While this could be done more cleanly
// with SFINAE (Substitution Failure Is Not An Error:
// http://en.cppreference.com/w/cpp/language/sfinae), MSVC
// 2013 made that awkward enough (though it is getting
// better:
// https://blogs.msdn.microsoft.com/vcblog/2015/12/02/partial-support-for-expression-sfinae-in-vs-2015-update-1/)
// to send us down this route instead.

// We'll use this down below in our definition of BaseInfo.
//
// Describe if the type should have a public constructor, a
// private one or should attach methods/free functions to an
// existing type.
enum class InstallType : char {
    Global = 0,
    Private,
    OverNative,
};

struct BaseInfo {
    // Indicates JS inheritance with the named type
    static const char* const inheritFrom;

    // If the constructor should be visible in the global
    // scope
    static const InstallType installType =
        InstallType::Global;

    static const JSFunctionSpec* freeFunctions;
    static const JSFunctionSpec* methods;

    static const unsigned classFlags = 0;

    // A special hook to run after the type is installed
    // into the scope
    static void postInstall(JSContext* cx,
                            JS::HandleObject global,
                            JS::HandleObject proto);

    static void addProperty(JSContext* cx,
                            JS::HandleObject obj,
                            JS::HandleId id,
                            JS::MutableHandleValue v);
    static void call(JSContext* cx, JS::CallArgs args);
    static void construct(JSContext* cx, JS::CallArgs args);
    static void convert(JSContext* cx,
                        JS::HandleObject obj,
                        JSType type,
                        JS::MutableHandleValue vp);
    static void delProperty(JSContext* cx,
                            JS::HandleObject obj,
                            JS::HandleId id,
                            bool* succeeded);
    static void enumerate(JSContext* cx,
                          JS::HandleObject obj,
                          JS::AutoIdVector& properties);
    static void finalize(JSFreeOp* fop, JSObject* obj);
    static void getProperty(JSContext* cx,
                            JS::HandleObject obj,
                            JS::HandleId id,
                            JS::MutableHandleValue vp);
    static void hasInstance(JSContext* cx,
                            JS::HandleObject obj,
                            JS::MutableHandleValue vp,
                            bool* bp);
    static void resolve(JSContext* cx,
                        JS::HandleObject obj,
                        JS::HandleId id,
                        bool* resolvedp);
    static void setProperty(JSContext* cx,
                            JS::HandleObject obj,
                            JS::HandleId id,
                            bool strict,
                            JS::MutableHandleValue vp);
};

// And we'll add some macros to clean up the interface a bit

// Declare the types that we'll need.  Implementation will
// go in AdaptedMyType::Functions::function::call.
#define DECLARE_JS_FUNCTION(function)        \
    struct function {                        \
        static const char* name() {          \
            return #function;                \
        }                                    \
        static void call(JSContext* cx,      \
                         JS::CallArgs args); \
    };

// Bear with me that we're constructing a JSFunctionSpec
// correctly
#define ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(name, ...) \
    {                                                    \
        #name, {wrapConstrainedMethod < Functions::name, \
                true,                                    \
                __VA_ARGS__ >,                           \
                nullptr },                               \
                0,                                       \
                0,                                       \
                nullptr                                  \
    }

// Giving us an AdaptedMyType header of:

struct AdaptedMyTypeInfo : public BaseInfo {
    static void construct(JSContext* cx, JS::CallArgs args);
    static void finalize(JSFreeOp* fop, JSObject* obj);

    struct Functions {
        DECLARE_JS_FUNCTION(toString);
        DECLARE_JS_FUNCTION(toNumber);
    };

    static const JSFunctionSpec methods[3];

    static const char* const className;
    static const unsigned classFlags = JSCLASS_HAS_PRIVATE;
};

// and an implementation of:

const JSFunctionSpec AdaptedMyTypeInfo::methods[3] = {
    ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(
        toNumber, AdaptedMyTypeInfo),
    ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(
        toString, AdaptedMyTypeInfo),
    JS_FS_END,
};

const char* const AdaptedMyTypeInfo::className = "MyType";

void AdaptedMyTypeInfo::construct(
    JSContext* cx, JS::CallArgs args) { /* ... */
}
void AdaptedMyTypeInfo::finalize(JSFreeOp* fop,
                                 JSObject* obj) { /* ... */
}
void AdaptedMyTypeInfo::Functions::toString::call(
    JSContext* cx, JS::CallArgs args) { /* ... */
}
void AdaptedMyTypeInfo::Functions::toNumber::call(
    JSContext* cx, JS::CallArgs args) { /* ... */
}

// We'll then adapt that with a wrapper that generates types
// from an appropriate policy:

template <typename T>
class WrapType : public T {
public:
    WrapType(JSContext* context);
    ~WrapType();

    // We'll break up the prototype installation into an
    // explicit step
    void install(JS::HandleObject global);

    // Create a new object without invoking the constructor
    void newObject(JS::MutableHandleObject out);

    // Create an object by invoking the constructor
    void newInstance(const JS::HandleValueArray& args,
                     JS::MutableHandleObject out);

    bool instanceOf(JS::HandleObject obj);

    const JSClass* getJSClass() const;

    JS::HandleObject getProto() const;
};

// Allowing us to create and install a new type by:

void myFunc(JSContext* cx, JS::HandleObject global) {
    WrapType<AdaptedMyTypeInfo> adaptedMyType(cx);

    adaptedMyType.install(global);
}

// While it may seem like a lot of work to save a little bit
// of boilerplate, a quick look at our codebase will show
// that we’ve needed to stamp out 25 instances of Wraptype
// and more than 75 wrapped functions.  While it was a bit
// of work to stand up, We’ve found that developers
// unfamiliar with this part of the codebase ramp fairly
// quickly and generally don’t need to do much more than to
// mimic existing examples.  Which, as the main maintainer
// of our JavaScript integration, is pretty much all I could
// have asked for.
//
// Note also that the solution presented at the end is
// almost exactly what we use today for our production
// JavaScript integration:
//
// The base policy:
// https://github.com/mongodb/mongo/blob/r3.2.4/src/mongo/scripting/mozjs/base.h
//
// The owning object for a MongoDB Javascript environment:
// https://github.com/mongodb/mongo/blob/r3.2.4/src/mongo/scripting/mozjs/implscope.h
//
// A number type very like the one described in this post:
// https://github.com/mongodb/mongo/blob/r3.2.4/src/mongo/scripting/mozjs/numberint.h
//
// A method constraining template described in this post:
// https://github.com/mongodb/mongo/blob/r3.2.4/src/mongo/scripting/mozjs/wrapconstrainedmethod.h
//
// The template library described in this post:
// https://github.com/mongodb/mongo/blob/r3.2.4/src/mongo/scripting/mozjs/wraptype.h
