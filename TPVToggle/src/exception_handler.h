#ifndef EXCEPTION_HANDLER_H
#define EXCEPTION_HANDLER_H

#include <windows.h>

// Exception handler for the INT3 breakpoint
LONG WINAPI ExceptionHandler(EXCEPTION_POINTERS *ExceptionInfo);

#endif // EXCEPTION_HANDLER_H
