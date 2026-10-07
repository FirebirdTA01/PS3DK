#ifndef ILP32_THUNK_VCALL_H
#define ILP32_THUNK_VCALL_H

struct Base
{
  int tag = 0x5a;
  virtual unsigned long long probe () = 0;
  virtual int value () = 0;
};

struct Derived : virtual Base
{
  int x = 0x1234;
  unsigned long long probe () override;
  int value () override;
};

Base *thunk_base (void);
Derived *thunk_derived (void);

#endif
