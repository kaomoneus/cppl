#ifndef with
#define with if
#endif

namespace levitation {
  // Simple testing framework.
  // * It is supposed to run integration tests for C++ Levitation compiler mode.
  // * It doesn't link any standard library and checks result is encoded into
  //   result returned by main() function.
  //
  // Structure is as follows:
  //
  // Test {
  // private:
  //   Utils {
  //     Messaging,
  //     Asserts, etc.
  //   }
  // public:
  //   Context {
  //     expect();
  //     open();
  //   }
  //   context(); // returns Context instance
  //   getResult(); // returns checks result.
  // }
  //
  // May be it is easier to consider Test and Utils as namespaces, for
  // those classes don't contain any non-static members.
  //
  // We use "class" instead of "namespaces" because:
  // * Tt allows to make some declarations private.
  // * It also helps to provide user with static methods by including
  //   this preamble, without need to compile separate object file.
  //
  // Example of use:
  //
  // In module you want to test:
  //
  //  // Module.cppl
  //  package namespace P {
  //    class A {
  //    public:
  //      void f() {
  //        levitation::Test::context()
  //        << "some message"
  //        << "some other message"
  //      }
  //    };
  //  }
  //
  // In main.cpp:
  //
  //  // main.cpp
  //  int main() {
  //    with (
  //      auto Test = levitation::Test::context()
  //          .expect("some message")
  //          .expect("some other message")
  //      .open()
  //    ) {
  //      // Run whatever you'd like to check
  //      P::A::f();
  //    }
  //    return levitation::Test::result();
  //  }

  class Test {
    class Utils {
    public:
      struct Assert {
        static void check(bool v, const char *msg) {
  #ifdef assert
          assert(v && msg);
  #endif
        }
      };

      enum struct MessageType {
          Empty,
          Text,
          Char,
          Integer
      };

      enum struct MessageCompareType {
          Equal,
          TypesNotEqual,
          ValuesNotEqual
      };

      struct Msg {
        Msg()
        : Type(Utils::MessageType::Empty)
        {}
        Msg(const char *v)
        : Type(Utils::MessageType::Text),
          Text(v)
        {}
        Msg(char v)
        : Type(Utils::MessageType::Char),
          Char(v)
        {}
        Msg(int v)
        : Type(Utils::MessageType::Integer),
          Integer(v)
        {}

        Utils::MessageType Type;
        union {
          const char *Text;
          char Char;
          int Integer;
        };

        MessageCompareType compare(const Msg &src) const {

          if (Type != src.Type)
            return MessageCompareType::TypesNotEqual;

          switch (Type) {
            case Utils::MessageType::Integer:
              if (Integer != src.Integer)
                return MessageCompareType::ValuesNotEqual;
              break;

            case Utils::MessageType::Text: {
                int i = 0;
                for (; Text[i] && src.Text[i]; ++i)
                  if (Text[i] != src.Text[i])
                    return MessageCompareType::ValuesNotEqual;
                // If strings are equal both sides at i should be \0.
                // Otherwise, strings are not equal.
                if (Text[i] != src.Text[i])
                  return MessageCompareType::ValuesNotEqual;
              }
              break;

            default:
              break;
              // do nothing
          }

          return MessageCompareType::Equal;
        }
      }; // end of Msg class

      template <int MESSAGES_MAX>
      class Messages {
        Msg Collection[MESSAGES_MAX];

        unsigned NextMessage = 0;

      public:

        inline const Msg& get(unsigned i) const {
          checkIndex(i);
          return Collection[i];
        }

        template <typename T>
        inline void add(T v) {
          checkIndex(NextMessage);
          Collection[NextMessage++] = v;
        }

        MessageCompareType compare(int i, const Msg &rhs) const {
          checkIndex(i);
          return Collection[i].compare(rhs);
        }

        unsigned size() const { return NextMessage; }

      protected:

        void checkIndex(unsigned i) const {
          Assert::check(
            i < MESSAGES_MAX,
            "Total amount of ActualMessages should be less that ActualMessages_MAX"
          );
        }
      }; // end of Messages class

      enum struct ItemError {
          NotEqual,
          Missed
      };

      // Result format is as follows.
      // Assuming Utils::ItemError enum consists of up to 2 values.
      // -1 - unknown
      //  0 - successfull.
      //  1..3 - reserved.
      //  2*(i + 1) + ItemCheckRes - check failed on item with index = i,
      //                            with result ItemCheckRes.
      class ResultFormatter {
      public:
        static constexpr unsigned getItemErrorBits() {
          return 1;
        }
        static constexpr int unknown() {
          return -1;
        }
        static constexpr int successfull() {
          return 0;
        }
        static constexpr int error(unsigned i, Utils::ItemError ItemCheckRes) {
          const unsigned factor = 1 << getItemErrorBits();
          return factor * (i + 1) + (int)ItemCheckRes;
        }
      };

    }; // end of Utils class

  public:

    class Context {

      Utils::Messages<16> Actual;
      Utils::Messages<16> Expected;

      bool Opened = false;

      int Res = Utils::ResultFormatter::unknown();

      Context() {};

      friend class Test;

    public:

      template <typename T>
      inline Context &operator<<(T src) {
        checkOpened();
        Actual.add(src);
        return *this;
      }

      template <typename T>
      Context &expect(T v) {
        Expected.add(v);
        return *this;
      }

      void check() {
        for (unsigned i = 0, e = Expected.size(); i != e; ++i) {
          if (Actual.size() <= i) {
            Res = Utils::ResultFormatter::error(i, Utils::ItemError::Missed);
            return;
          }

          switch (Expected.compare(i, Actual.get(i))) {
            case Utils::MessageCompareType ::ValuesNotEqual:
            case Utils::MessageCompareType ::TypesNotEqual:
              Res = Utils::ResultFormatter::error(i, Utils::ItemError::NotEqual);
              return;
            default:
              break;
          }
        }

        Res = Utils::ResultFormatter::successfull();
      }

      class OpenedContext {
        Context &C;
      public:
        OpenedContext(Context &c) : C(c) {}
        ~OpenedContext() {
          C.check();
        }
        operator bool() const {
          return true;
        }
      };

      OpenedContext open() {
        checkNotOpened();
        Opened = true;
        return OpenedContext(*this);
      }

    protected:
      void checkOpened() {
        Utils::Assert::check(Opened, "Test context must be opened.");
      }

      void checkNotOpened() {
        Utils::Assert::check(!Opened, "Test context must not be opened.");
      }
    }; // end of Context class

    static Context& context() {
      static Context context;
      return context;
    }

    static int result() {
      return context().Res;
    }
  }; // end of Test class
} // end of levitation::Test
