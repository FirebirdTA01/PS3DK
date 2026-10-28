/*! \file cell/ssl/error.h
 \brief Forwarder for the cellSsl error codes.

  The platform ABI ships the cellSsl error constants under a per-type
  header (cell/ssl/error.h).  Our SDK carries the whole cellSsl surface —
  functions and CELL_SSL_ERROR_* codes — in <cell/ssl/ssl.h>; this header
  bridges the include path.
*/

#ifndef PS3TC_CELL_SSL_ERROR_H
#define PS3TC_CELL_SSL_ERROR_H

#include <cell/ssl/ssl.h>

#endif  /* PS3TC_CELL_SSL_ERROR_H */
