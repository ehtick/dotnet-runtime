// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#pragma once

#include "pal_jni.h"
#include "pal_x509.h"

#include <pal_ssl_types.h>

typedef intptr_t ManagedContextHandle;
typedef void (*STREAM_WRITER)(ManagedContextHandle, uint8_t*, int32_t);
typedef int32_t (*STREAM_READER)(ManagedContextHandle, uint8_t*, int32_t*);
typedef void (*MANAGED_CONTEXT_CLEANUP)(ManagedContextHandle);

typedef struct SSLStream
{
    jobject sslContext;
    jobject sslEngine;
    jobject sslSession;
    jobject appOutBuffer;
    jobject netOutBuffer;
    jobject appInBuffer;
    jobject netInBuffer;
    ManagedContextHandle managedContextHandle;
    STREAM_READER streamReader;
    STREAM_WRITER streamWriter;
    MANAGED_CONTEXT_CLEANUP managedContextCleanup;
} SSLStream;

typedef struct ApplicationProtocolData_t ApplicationProtocolData;

// Matches managed PAL_SSLStreamStatus enum
enum
{
    SSLStreamStatus_OK = 0,
    SSLStreamStatus_NeedData = 1,
    SSLStreamStatus_Error = 2,
    SSLStreamStatus_Renegotiate = 3,
    SSLStreamStatus_Closed = 4,
};
typedef int32_t PAL_SSLStreamStatus;

/*
Create an SSL context

Returns NULL on failure
*/
PALEXPORT SSLStream* AndroidCryptoNative_SSLStreamCreate(intptr_t sslStreamProxyHandle);

/*
Create an SSL context with the specified certificates

Returns NULL on failure
*/
PALEXPORT SSLStream* AndroidCryptoNative_SSLStreamCreateWithCertificates(intptr_t sslStreamProxyHandle,
                                                                         uint8_t* pkcs8PrivateKey,
                                                                         int32_t pkcs8PrivateKeyLen,
                                                                         PAL_KeyAlgorithm algorithm,
                                                                         jobject* /*X509Certificate[]*/ certs,
                                                                         int32_t certsLen);

/*
Create an SSL context with the specified certificates and private key from KeyChain

Returns NULL on failure
*/
PALEXPORT SSLStream* AndroidCryptoNative_SSLStreamCreateWithKeyStorePrivateKeyEntry(intptr_t sslStreamProxyHandle, jobject privateKeyEntry);

/*
Initialize an SSL context
  - isServer                : true if the context should be created in server mode
  - streamReader            : callback for reading data from the connection
  - streamWriter            : callback for writing data to the connection
  - managedContextCleanup   : callback for cleaning up the managed context
  - appBufferSize           : initial buffer size for application data

Returns 1 on success, 0 otherwise
*/
PALEXPORT int32_t AndroidCryptoNative_SSLStreamInitialize(
    SSLStream* sslStream, bool isServer, ManagedContextHandle managedContextHandle, STREAM_READER streamReader, STREAM_WRITER streamWriter, MANAGED_CONTEXT_CLEANUP managedContextCleanup, int32_t appBufferSize, char* peerHost);

/*
Set target host
  - targetHost : SNI host name

Returns 1 on success, 0 otherwise
*/
PALEXPORT int32_t AndroidCryptoNative_SSLStreamSetTargetHost(SSLStream* sslStream, char* targetHost);

/*
Check if the local certificate has been sent to the peer during the TLS handshake.

Returns true if the local certificate has been sent to the peer, false otherwise.
*/
PALEXPORT bool AndroidCryptoNative_SSLStreamIsLocalCertificateUsed(SSLStream* sslStream);

/*
Start or continue the TLS handshake
*/
PALEXPORT PAL_SSLStreamStatus AndroidCryptoNative_SSLStreamHandshake(SSLStream* sslStream);

/*
Read bytes from the connection into a buffer
  - buffer : buffer to populate with the bytes read from the connection
  - length : maximum number of bytes to read
  - read   : [out] number of bytes read from the connection and written into the buffer

Unless data from a previous incomplete read is present, this will invoke the STREAM_READER callback.
*/
PALEXPORT PAL_SSLStreamStatus AndroidCryptoNative_SSLStreamRead(SSLStream* sslStream,
                                                                uint8_t* buffer,
                                                                int32_t length,
                                                                int32_t* read);
/*
Encodes bytes from a buffer
  - buffer : data to encode
  - length : length of buffer

This will invoke the STREAM_WRITER callback with the processed data.
*/
PALEXPORT PAL_SSLStreamStatus AndroidCryptoNative_SSLStreamWrite(SSLStream* sslStream, uint8_t* buffer, int32_t length);

/*
Release the SSL context
*/
PALEXPORT void AndroidCryptoNative_SSLStreamRelease(SSLStream* sslStream);

/*
Get the negotiated application protocol for the current session

Returns 1 on success, 0 otherwise
*/
PALEXPORT int32_t AndroidCryptoNative_SSLStreamGetApplicationProtocol(SSLStream* sslStream,
                                                                      uint8_t* out,
                                                                      int32_t* outLen);

/*
Get the name of the cipher suite for the current session

Returns 1 on success, 0 otherwise
*/
PALEXPORT int32_t AndroidCryptoNative_SSLStreamGetCipherSuite(SSLStream* sslStream, uint16_t** out);

/*
Get the standard name of the protocol for the current session (e.g. TLSv1.2)

Returns 1 on success, 0 otherwise
*/
PALEXPORT int32_t AndroidCryptoNative_SSLStreamGetProtocol(SSLStream* sslStream, uint16_t** out);

/*
Get the peer certificate for the current session

Returns the peer certificate or null if there is no peer certificate.
*/
PALEXPORT jobject /*X509Certificate*/ AndroidCryptoNative_SSLStreamGetPeerCertificate(SSLStream* sslStream);

/*
Get the peer certificates for the current session

The peer's own certificate will be first, followed by any certificate authorities.
*/
PALEXPORT void AndroidCryptoNative_SSLStreamGetPeerCertificates(SSLStream* sslStream,
                                                                jobject** /*X509Certificate[]*/ out,
                                                                int32_t* outLen);

/*
Configure the session to request client authentication
*/
PALEXPORT void AndroidCryptoNative_SSLStreamRequestClientAuthentication(SSLStream* sslStream);

/*
Set application protocols
  - protocolData : array of application protocols to set
  - count        : number of elements in protocolData
Returns 1 on success, 0 otherwise
*/
PALEXPORT int32_t AndroidCryptoNative_SSLStreamSetApplicationProtocols(SSLStream* sslStream,
                                                                       ApplicationProtocolData* protocolData,
                                                                       int32_t count);

/*
Set enabled protocols
  - protocols : array of protocols to enable
  - count     : number of elements in protocols

Returns 1 on success, 0 otherwise
*/
PALEXPORT int32_t AndroidCryptoNative_SSLStreamSetEnabledProtocols(SSLStream* sslStream,
                                                                   PAL_SslProtocol* protocols,
                                                                   int32_t count);

/*
Verify hostname using the peer certificate for the current session

Returns true if hostname matches, false otherwise
*/
PALEXPORT bool AndroidCryptoNative_SSLStreamVerifyHostname(SSLStream* sslStream, char* hostname);

/*
Shut down the session
*/
PALEXPORT bool AndroidCryptoNative_SSLStreamShutdown(SSLStream* sslStream);
