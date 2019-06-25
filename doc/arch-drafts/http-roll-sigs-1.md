# Partial content signing

  - Allow streaming content from origin to client.
      - Less memory usage in injector: do not "slurp" whole response.
      - Less latency to get beginning of content in client.
  - Allow streaming content from client to client.
      - Allow progressive validation of content: do not wait to full response (esp. if big like a video).
      - Allow clients to provide data from interrupted downloads (equivalent: some support for "infinite" responses).
  - Allow sending and validating partial content (i.e. HTTP ranges):
      - Injector-to-client
      - Client-to-client
  - Computing signatures is CPU-intensive.
      - Do not sign too small blocks.
      - Not too big either, since each block must be fully received before it can be validated and used.
  - Avoid replay attacks, also while streaming content:
      - Detect body data from an unrelated exchange, but signed by a trusted injector.
      - Detect blocks from the right exchange, but sent in wrong order or at the wrong offset.
  - If possible, reuse body data stored at the client.

This proposes an HTTP-based protocol to convey necessary information to fullfill the requirements above.

## Summary

The injector owns a **signing key pair** whose public key is known by the client as part of its configuration.

When the injector gets an injection request from the client, it gets the response head from the origin and sends a **partial response head** back to the client with relevant response headers, plus headers allowing other clients to validate the cached response when it is provided to them in future distributed cache lookups.  The later headers include:

  - The request URI (so that the response stands on its own).
  - The identifier of the injection and its time stamp (to tell this exchange apart from others of the same URI).
  - The key, algorithm and **data block length** used to sign partial data blocks.
  - A **partial signature** of the headers so far.

This signature is provided so that the partial response (head an body) can still be useful if the connection is interrupted later on.

The injector then sends data blocks of the maximum length specified above, each of them followed by a **data block signature** bound to this injection and its offset.  The client need not check the signatures but it can save them to provide them to other clients in case the connection to the injector is interrupted.

When all data blocks have been sent to the client, the injector sends additional headers to build the **final response head** including:

  - Content digests for the whole body.
  - The final content length.

HTTP chunked transfer encoding is used to enable providing a first set of headers, then a signature (as a chunk extension) after each sent block, then a final set of headers as a trailer.  The partial signature (and other headers related to transfer encoding) are not part of the final signature.

[Signing HTTP Messages][] is used here as a way to sign HTTP headers because of its simplicity, although other schemes may be used instead.

[Signing HTTP Messages]: https://datatracker.ietf.org/doc/html/draft-cavage-http-signatures-11

## Example of injection result

```
HTTP/1.1 200 OK
X-Oui-Version: 1
X-Oui-URI: https://example.com/foo
X-Oui-Injection: id=d6076384-2295-462b-a047-fe2c9274e58d,ts=1516048310
X-Oui-HTTP-Status: 200
Date: Mon, 15 Jan 2018 20:31:50 GMT
Server: Apache
Content-Type: text/html
Content-disposition: inline; filename="foo.html"
X-Oui-Hashing: keyId="????",algorithm="????",length=1048576
X-Oui-Part-Sig: keyId="????",algorithm="????",
  headers="x-oui-version x-oui-uri x-oui-injection x-oui-http-status date server content-type content-disposition x-oui-hashing",
  signature="BASE64(...)"
Transfer-Encoding: chunked
Trailer: Digest, X-Oui-Content-Length, X-Oui-Hashes, X-Oui-Hashes-Sig, X-Oui-Sig

80000
0123456789...
80000;s=BASE64(SIG(INJECTION_ID=d6076… SEP OFFSET=0x0 SEP BLOCK1))
0123456789...
4;s=BASE64(SIG(INJECTION_ID=d6076… SEP OFFSET=0x80000 SEP BLOCK2))
abcd
0;s=BASE64(SIG(INJECTION_ID=d6076… SEP OFFSET=0x100000 SEP BLOCK3))
Digest: SHA-256=BASE64(HASH_OF_FULL_BODY)
X-Oui-Content-Length: 1048580
X-Oui-Sig: keyId="????",algorithm="????",
  headers="x-oui-version x-oui-uri x-oui-injection x-oui-http-status date server content-type content-disposition x-oui-hashing digest x-oui-content-length",
  signature="BASE64(...)"
```

If the client sends an HTTP range request, the injector aligns it to block boundaries (this is acceptable according to [RFC7233#4.1][] — "a client cannot rely on receiving the same ranges that it requested").  The partial response head includes a signed ``Range:`` header, and hashes in the final response head correspond to the range, not the full data.

[RFC7233#4.1]: https://tools.ietf.org/html/rfc7233#section-4.1

Client-to-client transmission works in a similar way.

The signature for a given block comes in a chunk extension in the chunk right after the block's end (for the last block, in the final chunk), and it covers the injection identifier and block offset besides its content.  This avoids replay and reordering attacks, but it also binds the stream representation to this injection.  Storage that keeps signatures inline with block data should take this into account.

## Issues

  - Choose an adequate data block length (can use different ones according to ``Content-Length:``).
  - Choose block signature algorithm.
  - It may only be usable for single ranges (i.e. no ``multipart/byteranges`` body).
  - Block hashes are outside of the signed HTTP head.  Inlining them in the final head may require long Base64-encoded headers for long files.
