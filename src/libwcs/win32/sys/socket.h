/*
 * Windows MSVC stub for <sys/socket.h>: forwards to Winsock2.
 * Winsock2 provides BSD-compatible socket()/connect()/send()/recv() and
 * the sockaddr / sockaddr_in struct definitions that libwcs's webread.c uses.
 *
 * The wcs target links ws2_32 on Windows so these symbols resolve at link time.
 */
#ifndef VISIVO_LIBWCS_WIN32_SYS_SOCKET_H
#define VISIVO_LIBWCS_WIN32_SYS_SOCKET_H
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
