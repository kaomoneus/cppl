[toc]

# C++ Levitation Units
This is an extension to C++17, and it introduces original
[modularity](https://en.wikipedia.org/wiki/Modular_programming)
support for C++.

## Basic concepts
Levitation Packages is a replacement for _C/C++_ `#include` directives.
In _C++ Levitation_ mode the latter is still supported, but only as landing pad for legacy C++ code.

## Simplest things

### Example 1

Let's consider simple example program with one package
and two classes.

#### Defining units

_**MyPackage/UnitA.cppl**_

```cpp
#include <iostream>
class A {
public:
  static void sayHello() {
    std::cout << "Hello!\n";
  }
};
```

It looks pretty much like a regular C++ class. But intead it is a defined as a Levitation Unit. Which implicitly sorrounds by a namespace `MyPackage::UnitA`.

To demonstrate that, let's take look on another class which will use the first one.

_**MyPackage/UnitB.cppl**_

```cpp
#import MyPackage::UnitA
class B {
public:
  static void useA() {
    MyPackage::UnitA::A::sayHello();
  }
};
```

Here we import "MyPackage::UnitA" and can use its contents.

Fanally we want to compile it into application. So we've got to add main function somewhere. But how?

Indeed everything in unit implicitly surrounded by namespace scopes. Whilst "main" should be defined at global scope.

There is another special thing in C++ Levitation.

#### Defining global namespace

It is possible to define "global" namespace in units. It is acheived by `namespace :: {...}` syntax.

So here's how we can define a main function.

_**main.cpp**_

```cpp

#import MyPackage::UnitB

namespace :: { // enter global namespace
  int main() {
    MyPackage::UnitB::B::useA();
      return 0;
    }
  }
}
```

#### Gathering together

In this example we have introduced two classes `A` and `B`, both belong to
same package `MyPackage`, but to different units. Namely `MyPackage::UnitA` for `A` class and `MyPackage::UnitB` for `B` class.
`B` calls static method `MyPackage::UnitA::A::sayHello()` of `A` class. 
 
In order to tell compiler that `MyPackage::UnitB` depends on `MyPackage::UnitA`, we added
`#import` directive in top of _UnitA.cppl_ file.

In our example we just call `MyPackage::B::useA()` and then return `0`.

**Note:** `#include` directives are also supported. But whenever programmer
uses them, current package and all dependent packages will
include whatever `#include` directive refers to. So it is always better to use `#import` directive whenever its possible.

# Special cases

Regular C++ allows to separate declaration from definition. You can include declaration everywhere, whilst definition is restricted by [ODR rule](https://en.wikipedia.org/wiki/One_Definition_Rule) and you should put it into separate file.

In C++ Levitation it is done semiautomatically. We say "semi" because we keep possibility to notify compiler whenever things are to be considered as definition. We also changed `inline` methods treatment.

## _Inline_ methods
In regular C++ inline methods are implicitly inlined. So if you include declaration with inline methods in several different definitions you'll get same code inlined into several places.

In C++ Levitation all methods are considered as non-inline and
externally visible, unless `inline` is not specified.

```cpp
  class A {
  public:
    // Method below is non-inline and static, only its prototype will be
    // visible for other units.
    static void availableExternally() {
      // ...
    }

    // Method below is non-inline and non-static, same here, only
    // its prototype will be visible for other units.
    void availableExternallyToo() {
      // ...
    }
    
    // Method below inline and static. Regular inline rules applied.
    // Whenever you call it in other units whole its contents
    // copied into caller's body.
    static inline void inlineStaticMethod() {
      // ...
    }   
  };
```

## The notion of _body_, `#body` directive

This is how we allow to notify compiler about _definition_ part of unit.

```cpp

class A {
public:
  // Non-inline method with in-place definition.
  static void inPlace() {
    // ...
  }

  // Method with external definition
  static void externallyDefined();
};
  
#body

void A::externallyDefined() {
  // Method definition
} 
```

### _Round-trip_ dependencies

Consider two classes `A` and `B`.

* Class `B`  somehow refers to `A`.
* While class `A` in its definition parts also refers to class `B`.

This is a circular dependency.

_**MyPackage/UnitB.cppl**_

```cpp
#include <iostream>
#import MyPackage::UnitA
class B {
public:
  static void useA() {
    MyPackage::UnitA::A::sayHello();
  }
  
  static void sayer(const char* msg) {
    std::cout << msg << std::endl;
  }
};
```

_**MyPackage/UnitA.cppl (wrong)**_

```cpp
#import MyPackage::UnitB // error, circular dependency
class A {
public:
  static void sayHello() {
    MyPackage::UnitB::B::sayer("Hello"); // attempt to use 'B' in body of 'A'
  }
};
```

In C++ it is resolved by implicit separation onto _.h_ and _.cpp_ files.

In C++ Levitation we also separate units onto declaration and definition, but it is done by _cppl_ compiler automatically.
It is possible though to control where exactly you want to import unit.

#### `[bodydep]` attribute for `#import`

_`[bodydep]`_ says to compiler, that we want to import unit into *definition part only*.


_**MyPackage/UnitA.cppl (good)**_

```cpp
#import [bodydep] MyPackage::UnitB // OK
class A {
public:
  static void sayHello() {
    MyPackage::UnitB::B::sayer("Hello"); // attempt to use 'B' in body of 'A'
  }
};
```

Here there will be no circular dependency. 

* _UnitA_ consists of two nodes: declaration and definition
* Same for _UnitB_

So in current example we defined:
* _UnitB_ definition depends on
   * _UnitB_ declaration
   * _UnitA_ declaration
* _UnitA_ definition depends on
   * _UnitA_ declaration
   * _UnitB_ declaration
* _UnitB_ declaration depends on
   * _UnitA_ declaration
* _UnitA_ declaration has no dependencies

[See illustration on paste.pics](https://paste.pics/9J3R1)

So after all there are no cycles.

Another way to import unit only for _body_ part is to put it after `#body` directive. In this case `[bodydep]` attribute is not required.

_**MyPackage/UnitA.cppl (good, 2nd option)**_

```cpp
class A {
public:
  static void sayHello() {
    MyPackage::UnitB::B::sayer("Hello"); // attempt to use 'B' in body of 'A'
  }
};
#body
#import MyPackage::UnitB // OK
```

## Project structure, limitations
> _Good laws are limitations of our worst to release our best._

In C++ Levitation mode use of File System is restricted.

Source file path corresponds to its unit location.

For example, unit `com::MyOuterScope::MyPackage` should be located at path
`<project-root>/com/MyOuterScope/MyPackage.cppl`.

## Getting started
### Getting C++ Levitation Compiler from sources
C++ Levitation Compiler implementation is based on LLVM Clang frontend.
 
1. Clone C++ Levitation repository 
   ```bash
   git clone https://github.com/kaomoneus/cppl.git cppl
   cd cppl
   git checkout levitation-master
   ```
2. Create directory for binaries, for example '../cppl.build'
3. `cd ../cppl.build`
4. Run _cmake_ (assuming you want use 'cppl.instal' as directory
with installed binaries, and 'cppl' is accessable as '../cppl').

	```sh
	cmake -DLLVM_ENABLE_PROJECTS=clang \
	      -DCMAKE_INSTALL_PREFIX=cppl.install \
	      -G "Unix Makefiles" ../cppl
	```

5. `make`
6. `make check-clang`
7. `make install`
8. `alias cppl=<path-to-cppl.install>\bin\cppl`

### How to build executable with C++ Levitation Compiler

In order to build C++ Levitation code user should provide compiler
with following information:

* Project root directory (by default it is current directory). 
* Number of parallel jobs. Usually it is double of number of
available CPU cores.
* Name of output file, by default is 'a.out'.

Consider we want to compile project located at directory 'my-project'
with `main` located at 'my-project/my-project.cpp'.

Assuming we have a quad-core CPU we should run command:

`cppl -root="my-project" -j8 -o app.out`

If user is not fine with long and complicated command-lines, then she could rename
'my-project.cpp' to 'main.cpp' and change directory to 'my-project'.

Then build command could be reduced to

`cppl -j8 -o app.out`

Or even just

`cppl`

In latter case compiler will use single thread compilation and saves
executable as _a.out_.

### Building library
_Related tasks: L-4, L-27_

Just like a traditional C++ compilers, `cppl` produces set of object
files.

Library creation is a bit out of compilers competence.

But, it is possible to inform compiler that we need object files
to be saved somewhere for future use.

As long as we working with non-standard C++ source code,
we also need to generate .h file with all exported declarations.

_Or if we going to use it with other C++ Levitation project it will also generate .cppl declaration files._

Finally, we obtain set of object files, a set of regular C++ .h files, and as an alternative set of truncated .cppl files.
Having this at hands it is possible to create library with standard tools.

For example, building static library with _gcc_ tools and _Bash_ consists of
2 steps (assuming current directory is project root, and compiler uses single
thread):

1. `cppl -h=my-project.h -c=lib-objects`
2. `ar rcs my-project.a $(ls lib-objects/*.o)`

The only difference to regular C++ approach is step 1. On this step
we ask `cppl` to produce legacy object files and .h file.

* `-h=<filename>` asks compiler to generate C++ header file, and save
it with _'\<filename\>'_ name.
* `-c=<directory>` asks compiler to produce object files and store them
in directory with _'\<directory\>'_ name. It also tells compiler,
that there is no main file. Theoretically
it is still possible to declare `int main()` somewhere though.

On step 2 `ar` tool is instructed to create a static library `my-project.a`
and include into it all objects from `lib-objects` directory. 

## Theory of operation. Manual build (TODO)
