// This is a generated file. Don't edit it.
// Edit main.cpp.in and use bash.sh or test-all.sh
// to generate it again.
// ------------------------------------------------

// RUN:  %clang -cc1 -std=c++17 -xc++ -levitation-build-preamble -DCOMPILE_PCH %S/preamble.hpp -o %T/preamble.pch
// RUN:  %clang -cc1 -std=c++17 -xc++ %S/preamble.hpp -emit-obj -o %T/preamble.o
// Parsing 'P1/B'...
// RUN:  %clang -cc1 -std=c++17 -levitation-preamble=%T/preamble.pch -xc++ -levitation-build-ast -levitation-sources-root-dir=%S -levitation-deps-output-file=%T/P1_B.ldeps %S/P1/B.cppl -o %T/P1_B.ast
// Parsing 'P1/C'...
// RUN:  %clang -cc1 -std=c++17 -levitation-preamble=%T/preamble.pch -xc++ -levitation-build-ast -levitation-sources-root-dir=%S -levitation-deps-output-file=%T/P1_C.ldeps %S/P1/C.cppl -o %T/P1_C.ast
// Parsing 'P1/D'...
// RUN:  %clang -cc1 -std=c++17 -levitation-preamble=%T/preamble.pch -xc++ -levitation-build-ast -levitation-sources-root-dir=%S -levitation-deps-output-file=%T/P1_D.ldeps %S/P1/D.cppl -o %T/P1_D.ast
// Instantiating 'P1/B'...
// RUN:  %clang -cc1 -std=c++17 -levitation-preamble=%T/preamble.pch -flevitation-build-decl -emit-pch %T/P1_B.ast -o %T/P1_B.decl-ast
// Instantiating 'P1/C'...
// RUN:  %clang -cc1 -std=c++17 -levitation-preamble=%T/preamble.pch -flevitation-build-decl -levitation-dependency=%T/P1_B.ast -levitation-dependency=%T/P1_B.decl-ast -emit-pch %T/P1_C.ast -o %T/P1_C.decl-ast
// Instantiating 'P1/D'...
// RUN:  %clang -cc1 -std=c++17 -levitation-preamble=%T/preamble.pch -flevitation-build-decl -levitation-dependency=%T/P1_B.ast -levitation-dependency=%T/P1_B.decl-ast -levitation-dependency=%T/P1_C.ast -levitation-dependency=%T/P1_C.decl-ast -emit-pch %T/P1_D.ast -o %T/P1_D.decl-ast
// Compiling 'P1/B'...
// RUN:  %clang -cc1 -std=c++17 -levitation-preamble=%T/preamble.pch -flevitation-build-object -emit-obj %T/P1_B.ast -o %T/P1_B.o
// Compiling 'P1/C'...
// RUN:  %clang -cc1 -std=c++17 -levitation-preamble=%T/preamble.pch -flevitation-build-object -emit-obj -levitation-dependency=%T/P1_B.ast -levitation-dependency=%T/P1_B.decl-ast %T/P1_C.ast -o %T/P1_C.o
// Compiling 'P1/D'...
// RUN:  %clang -cc1 -std=c++17 -levitation-preamble=%T/preamble.pch -flevitation-build-object -emit-obj -levitation-dependency=%T/P1_B.ast -levitation-dependency=%T/P1_B.decl-ast -levitation-dependency=%T/P1_C.ast -levitation-dependency=%T/P1_C.decl-ast %T/P1_D.ast -o %T/P1_D.o
// Compiling source 'main.cpp'...
// RUN:  %clang -cc1 -std=c++17 -levitation-preamble=%T/preamble.pch -xc++ -flevitation-build-object -emit-obj -levitation-dependency=%T/P1_B.ast -levitation-dependency=%T/P1_B.decl-ast -levitation-dependency=%T/P1_C.ast -levitation-dependency=%T/P1_C.decl-ast -levitation-dependency=%T/P1_D.ast -levitation-dependency=%T/P1_D.decl-ast %S/main.cpp -o %T/main.o
// RUN:  %clangxx %T/main.o %T/preamble.o %T/P1_B.o %T/P1_C.o %T/P1_D.o -o %T/app.out
// RUN:  %T/app.out
int main() {
  // This test doesn't require any Test::context() checks.
  // The app just should be compiled and ran.
  P1::D::f();
  return 0;
}
