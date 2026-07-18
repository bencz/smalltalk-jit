"=== Standard test harness. Paste verbatim at the top of each tests/*.st file.
 Every check is wrapped so an unexpected runtime error counts as a FAILURE
 (a raw Assert failure or uncaught error prints a backtrace yet still exits 0,
 i.e. a false pass). The final top-level block MUST end with ^t report, whose
 integer return becomes the process exit code (0 = pass) that run_tests.sh
 gates on. Do not use the old Assert class in new tests. All catches use
 Exception (the root) so a wrong error-class name surfaces as a visible FAIL,
 never a silent exit-0. ==="
TestRun := Object [
    | fails count name |
    class named: aString [ ^self new setName: aString ]
    setName: aString [ name := aString. fails := 0. count := 0. ^self ]
    fail: aLabel [
        fails := fails + 1.
        Transcript nextPutAll: name; nextPutAll: ' FAIL: '; nextPutAll: aLabel; lf.
    ]
    error: aLabel from: e [
        fails := fails + 1.
        Transcript nextPutAll: name; nextPutAll: ' ERROR ['; nextPutAll: aLabel;
            nextPutAll: ']: '; nextPutAll: e messageText; lf.
    ]
    check: aLabel that: aBlock [
        count := count + 1.
        [ (aBlock value = true) ifFalse: [ self fail: aLabel ] ]
            on: Exception do: [:e | self error: aLabel from: e ].
    ]
    check: aLabel equals: expected is: aBlock [
        count := count + 1.
        [ | got |
          got := aBlock value.
          got = expected ifFalse: [
              self fail: aLabel, ' (got ', got printString, ' expected ', expected printString, ')' ] ]
            on: Exception do: [:e | self error: aLabel from: e ].
    ]
    expect: aLabel raises: aClass in: aBlock [
        count := count + 1.
        [ aBlock value. self fail: aLabel, ' (no error raised)' ]
            on: Exception do: [:e |
                (aClass notNil and: [ e isKindOf: aClass ]) ifFalse: [
                    self fail: aLabel, ' (raised ', e class printString, ' not ', aClass printString, ')' ] ].
    ]
    report [
        Transcript nextPutAll: name; nextPutAll: ': ';
            nextPutAll: (count - fails) printString; nextPutAll: '/';
            nextPutAll: count printString; nextPutAll: ' ok'; lf.
        ^fails
    ]
]

"=== Test-only helpers (no sockets, no network). FakeSocket satisfies the two
 socket contracts the framework actually uses off the wire:
   read side  : directNext:into:startingAt: (what HttpConnection>>readMore calls)
   write side : nextPutAll:count:            (what HttpResponse>>writeOn: calls)
 so both the server-side request parser and the client-side response parser,
 plus the response renderer, run fully in memory. Wire builds raw CRLF bytes.

 NOTE ON STRUCTURE: the VM caps each block/method scope at 256 native handles
 (vm/core/Handle.h). A single driver block with ~110+ statements overflows it
 and aborts, so (like the existing JsonTest.st) the checks are split across
 several top-level blocks that share ONE TestRun via the global HttpT; the last
 block ends with ^HttpT report. NO emoji, NO em-dash (hard project rule). ==="

FakeSocket := Object [
    | inData inPos out |
    class on: aString [ ^self new setIn: aString ]
    setIn: aString [
        inData := aString.
        inPos := 1.
        out := CollectionStream with: (String new: 256).
        ^self
    ]
    "Read side: copy up to n bytes of the pending input into aBuffer, return the
     count actually copied; 0 once the input is exhausted (peer closed)."
    directNext: n into: aBuffer startingAt: start [
        | count |
        count := 0.
        [ inPos <= inData size and: [ count < n ] ] whileTrue: [
            aBuffer at: start + count put: (inData at: inPos).
            inPos := inPos + 1.
            count := count + 1 ].
        ^count
    ]
    "Write side: HttpResponse hands us its backing buffer plus a valid length."
    nextPutAll: aString count: anInteger [
        out nextPutAll: (aString copyFrom: 1 to: anInteger).
        ^self
    ]
    written [ ^out contents ]
    close [ ^self ]
]

Wire := Object [
    class crlf [
        | s |
        s := String new: 2.
        s at: 1 put: Character cr.
        s at: 2 put: Character lf.
        ^s
    ]
    "Assemble a raw request/response: first line, header lines, blank line, body."
    class message: firstLine headers: heads body: aBody [
        | out cr |
        cr := self crlf.
        out := CollectionStream with: (String new: 128).
        out nextPutAll: firstLine; nextPutAll: cr.
        heads do: [:h | out nextPutAll: h; nextPutAll: cr ].
        out nextPutAll: cr.
        aBody notNil ifTrue: [ out nextPutAll: aBody ].
        ^out contents
    ]
    "Run the server-side parser over an in-memory request."
    class readRequest: aString [
        ^(HttpConnection on: (FakeSocket on: aString)) readRequest
    ]
    "Run the client-side parser over an in-memory response."
    class readResponse: aString [
        ^HttpResponse readFrom: (HttpConnection on: (FakeSocket on: aString))
    ]
    "True when needle occurs anywhere in haystack."
    class in: haystack has: needle [
        ^(haystack indexOfSubCollection: needle startingAt: 1) > 0
    ]
    "Render a response to a string via the fake write socket."
    class render: aResponse keepAlive: aBool [
        | fs |
        fs := FakeSocket on: ''.
        aResponse writeOn: fs keepAlive: aBool.
        ^fs written
    ]
]

"==================== HttpRequest: direct construction (1/2) ===================="
[
    HttpT := TestRun named: 'Http'.

    HttpT check: 'method is uppercased' equals: 'GET' is: [
        (HttpRequest new setMethod: 'get' target: '/x' version: 'HTTP/1.1') method ].
    HttpT check: 'path with no query' equals: '/a/b/c' is: [
        (HttpRequest new setMethod: 'GET' target: '/a/b/c' version: 'HTTP/1.1') path ].
    HttpT check: 'version is retained' equals: 'HTTP/1.1' is: [
        (HttpRequest new setMethod: 'GET' target: '/x' version: 'HTTP/1.1') version ].
    HttpT check: 'query string: first param' equals: 'hello' is: [
        (HttpRequest new setMethod: 'GET' target: '/s?q=hello&n=2' version: 'HTTP/1.1') query: 'q' ].
    HttpT check: 'query string: second param' equals: '2' is: [
        (HttpRequest new setMethod: 'GET' target: '/s?q=hello&n=2' version: 'HTTP/1.1') query: 'n' ].
    HttpT check: 'query path stripped of query part' equals: '/s' is: [
        (HttpRequest new setMethod: 'GET' target: '/s?q=hello' version: 'HTTP/1.1') path ].
    HttpT check: 'query url-decode: plus becomes space' equals: 'a b' is: [
        (HttpRequest new setMethod: 'GET' target: '/s?q=a+b' version: 'HTTP/1.1') query: 'q' ].
    HttpT check: 'query url-decode: percent-hex' equals: 'A' is: [
        (HttpRequest new setMethod: 'GET' target: '/s?c=%41' version: 'HTTP/1.1') query: 'c' ].
    HttpT check: 'query: flag with no equals decodes to empty string' equals: '' is: [
        (HttpRequest new setMethod: 'GET' target: '/s?flag&x=1' version: 'HTTP/1.1') query: 'flag' ].
    HttpT check: 'query is nil when no query string' that: [
        (HttpRequest new setMethod: 'GET' target: '/s' version: 'HTTP/1.1') query isNil ].
    HttpT check: 'query: on request with no query answers nil' that: [
        ((HttpRequest new setMethod: 'GET' target: '/s' version: 'HTTP/1.1') query: 'x') isNil ].
    HttpT check: 'pathSegments splits on slash' equals: 3 is: [
        (HttpRequest new setMethod: 'GET' target: '/a/b/c' version: 'HTTP/1.1') pathSegments size ].
]

"==================== HttpRequest: direct construction (2/2) ===================="
[
    HttpT check: 'pathSegments first element' equals: 'a' is: [
        (HttpRequest new setMethod: 'GET' target: '/a/b/c' version: 'HTTP/1.1') pathSegments first ].
    HttpT check: 'param: round-trips a bound value' equals: '42' is: [
        | r | r := HttpRequest new. r param: 'id' put: '42'. r param: 'id' ].
    HttpT check: 'param: on unbound request answers nil' that: [
        ((HttpRequest new) param: 'nope') isNil ].
    HttpT check: 'header: is case-insensitive (upper query)' equals: 'text/plain' is: [
        | r | r := HttpRequest new. r addRawHeaderLine: 'Content-Type: text/plain'. r header: 'CONTENT-TYPE' ].
    HttpT check: 'materialised header dict has lowercased key' equals: 'text/plain' is: [
        | r | r := HttpRequest new. r addRawHeaderLine: 'Content-Type: text/plain'. r headers at: 'content-type' ].
    HttpT check: 'header value is trimmed of surrounding spaces' equals: 'v' is: [
        | r | r := HttpRequest new. r addRawHeaderLine: 'X-Trim:    v   '. r header: 'x-trim' ].
    HttpT check: 'absent header answers nil' that: [
        | r | r := HttpRequest new. r addRawHeaderLine: 'A: b'. (r header: 'missing') isNil ].
    HttpT check: 'contentLength from raw header' equals: 27 is: [
        | r | r := HttpRequest new. r addRawHeaderLine: 'Content-Length: 27'. r contentLength ].
    HttpT check: 'contentLength defaults to 0' equals: 0 is: [ (HttpRequest new) contentLength ].
    HttpT check: 'HTTP/1.1 keep-alive by default' that: [
        (HttpRequest new setMethod: 'GET' target: '/' version: 'HTTP/1.1') isKeepAlive ].
    HttpT check: 'HTTP/1.1 Connection: close closes' that: [
        | r | r := HttpRequest new setMethod: 'GET' target: '/' version: 'HTTP/1.1'.
        r addRawHeaderLine: 'Connection: close'. r isKeepAlive not ].
    HttpT check: 'HTTP/1.0 closes by default' that: [
        (HttpRequest new setMethod: 'GET' target: '/' version: 'HTTP/1.0') isKeepAlive not ].
    HttpT check: 'HTTP/1.0 Connection: keep-alive stays open' that: [
        | r | r := HttpRequest new setMethod: 'GET' target: '/' version: 'HTTP/1.0'.
        r addRawHeaderLine: 'Connection: keep-alive'. r isKeepAlive ].
]

"==================== HttpRequest: body + parsed from raw bytes ===================="
[
    HttpT check: 'body set and read back' equals: 'payload' is: [
        (HttpRequest new setBody: 'payload'; body) ].
    HttpT check: 'jsonBody parses a JSON body' equals: 'Bob' is: [
        | r | r := HttpRequest new setBody: '{"name":"Bob"}'. r jsonBody at: 'name' ].
    HttpT check: 'jsonBody of empty body is nil' that: [ (HttpRequest new) jsonBody isNil ].
    HttpT expect: 'jsonBody of malformed JSON raises JsonParseError' raises: JsonParseError in: [
        | r | r := HttpRequest new setBody: 'not json'. r jsonBody ].
    HttpT check: 'parsed GET: method' equals: 'GET' is: [
        (Wire readRequest: (Wire message: 'GET /cards/7?x=hi HTTP/1.1' headers: #('Host: localhost') body: nil)) method ].
    HttpT check: 'parsed GET: path' equals: '/cards/7' is: [
        (Wire readRequest: (Wire message: 'GET /cards/7?x=hi HTTP/1.1' headers: #('Host: localhost') body: nil)) path ].
    HttpT check: 'parsed GET: version' equals: 'HTTP/1.1' is: [
        (Wire readRequest: (Wire message: 'GET /cards/7?x=hi HTTP/1.1' headers: #('Host: localhost') body: nil)) version ].
    HttpT check: 'parsed GET: query param' equals: 'hi' is: [
        (Wire readRequest: (Wire message: 'GET /cards/7?x=hi HTTP/1.1' headers: #('Host: localhost') body: nil)) query: 'x' ].
    HttpT check: 'parsed GET: header accessible' equals: 'localhost' is: [
        (Wire readRequest: (Wire message: 'GET / HTTP/1.1' headers: #('Host: localhost') body: nil)) header: 'host' ].
    HttpT check: 'parsed POST: method' equals: 'POST' is: [
        (Wire readRequest: (Wire message: 'POST /echo HTTP/1.1' headers: #('Content-Length: 14') body: '{"name":"Bob"}')) method ].
    HttpT check: 'parsed POST: body captured by content-length' equals: '{"name":"Bob"}' is: [
        (Wire readRequest: (Wire message: 'POST /echo HTTP/1.1' headers: #('Content-Length: 14') body: '{"name":"Bob"}')) body ].
    HttpT check: 'parsed POST: jsonBody decodes the body' equals: 'Bob' is: [
        (Wire readRequest: (Wire message: 'POST /echo HTTP/1.1' headers: #('Content-Length: 14') body: '{"name":"Bob"}')) jsonBody at: 'name' ].
]

"==================== HttpRequest parser edge cases + HttpResponse building (1/2) ===================="
[
    HttpT check: 'parser tolerates a leading blank line' equals: 'GET' is: [
        (Wire readRequest: (Wire crlf, (Wire message: 'GET / HTTP/1.1' headers: #() body: nil))) method ].
    HttpT check: 'malformed request line (no space) yields nil' that: [
        (Wire readRequest: (Wire message: 'GARBAGE' headers: #() body: nil)) isNil ].
    HttpT check: 'request line without version defaults to HTTP/1.0' equals: 'HTTP/1.0' is: [
        (Wire readRequest: (Wire message: 'GET /only-two-fields' headers: #() body: nil)) version ].
    HttpT check: 'parsed request keep-alive reflects Connection header' that: [
        (Wire readRequest: (Wire message: 'GET / HTTP/1.1' headers: #('Connection: close') body: nil)) isKeepAlive not ].
    HttpT check: 'ok status' equals: 200 is: [ (HttpResponse ok) status ].
    HttpT check: 'ok reason' equals: 'OK' is: [ (HttpResponse ok) reason ].
    HttpT check: 'status: recomputes the reason phrase' equals: 'Not Found' is: [
        (HttpResponse status: 404) reason ].
    HttpT check: 'notFound status' equals: 404 is: [ (HttpResponse notFound) status ].
    HttpT check: 'notFound body carries error message' equals: 'not found' is: [
        (HttpResponse notFound) jsonBody at: 'error' ].
    HttpT check: 'created: status' equals: 201 is: [
        (HttpResponse created: (Dictionary new at: 'x' put: 1; yourself)) status ].
    HttpT check: 'created: encodes the object as the body' equals: 1 is: [
        (HttpResponse created: (Dictionary new at: 'x' put: 1; yourself)) jsonBody at: 'x' ].
]

"==================== HttpResponse building (2/2) ===================="
[
    HttpT check: 'noContent status' equals: 204 is: [ (HttpResponse noContent) status ].
    HttpT check: 'badRequest: status' equals: 400 is: [ (HttpResponse badRequest: 'bad') status ].
    HttpT check: 'badRequest: message' equals: 'bad' is: [ (HttpResponse badRequest: 'bad') jsonBody at: 'error' ].
    HttpT check: 'methodNotAllowed status' equals: 405 is: [ (HttpResponse methodNotAllowed) status ].
    HttpT check: 'serverError: status' equals: 500 is: [ (HttpResponse serverError: 'boom') status ].
    HttpT check: 'serverError: message' equals: 'boom' is: [ (HttpResponse serverError: 'boom') jsonBody at: 'error' ].
    HttpT check: 'reasonFor: unknown code falls back to OK' equals: 'OK' is: [ HttpResponse reasonFor: 799 ].
    HttpT check: 'json: sets application/json content type' equals: 'application/json' is: [
        | r | r := (HttpResponse ok) json: (Dictionary new at: 'a' put: 1; yourself). r headers at: 'Content-Type' ].
    HttpT check: 'json: sets the encoded body' equals: 1 is: [
        | r | r := (HttpResponse ok) json: (Dictionary new at: 'a' put: 1; yourself). r jsonBody at: 'a' ].
    HttpT check: 'text: sets text/plain content type' equals: 'text/plain; charset=utf-8' is: [
        ((HttpResponse ok) text: 'hi') headers at: 'Content-Type' ].
    HttpT check: 'text: sets the raw body' equals: 'hi' is: [ ((HttpResponse ok) text: 'hi') body ].
    HttpT check: 'header:put: stores into the header dict' equals: 'v' is: [
        ((HttpResponse ok) header: 'X-Test' put: 'v') headers at: 'X-Test' ].
    HttpT check: 'response jsonBody of empty body is nil' that: [ (HttpResponse ok) jsonBody isNil ].
]

"==================== HttpResponse: rendering (writeOn:) ===================="
[
    HttpT check: 'render: status line first' equals: 1 is: [
        (Wire render: (HttpResponse ok) keepAlive: true) indexOfSubCollection: 'HTTP/1.1 200 OK' startingAt: 1 ].
    HttpT check: 'render: 404 status line with reason' that: [
        Wire in: (Wire render: (HttpResponse notFound) keepAlive: false) has: 'HTTP/1.1 404 Not Found' ].
    HttpT check: 'render: content-type header present' that: [
        Wire in: (Wire render: ((HttpResponse ok) text: 'hi') keepAlive: true) has: 'Content-Type: text/plain; charset=utf-8' ].
    HttpT check: 'render: content-length equals body size' that: [
        Wire in: (Wire render: ((HttpResponse ok) text: 'hello') keepAlive: true) has: 'Content-Length: 5' ].
    HttpT check: 'render: keep-alive connection header' that: [
        Wire in: (Wire render: (HttpResponse ok) keepAlive: true) has: 'Connection: keep-alive' ].
    HttpT check: 'render: close connection header' that: [
        Wire in: (Wire render: (HttpResponse ok) keepAlive: false) has: 'Connection: close' ].
    HttpT check: 'render: body appears in output' that: [
        Wire in: (Wire render: ((HttpResponse ok) text: 'the-body') keepAlive: true) has: 'the-body' ].
    HttpT check: 'render: custom header emitted' that: [
        | r | r := (HttpResponse ok) header: 'X-Custom' put: 'yes'.
        Wire in: (Wire render: r keepAlive: true) has: 'X-Custom: yes' ].
    HttpT check: 'render: integer header value printed' that: [
        | r | r := (HttpResponse ok) header: 'X-Count' put: 42.
        Wire in: (Wire render: r keepAlive: true) has: 'X-Count: 42' ].
    HttpT check: 'render: user Connection value overridden by framework' that: [
        | r out | r := ((HttpResponse ok) text: 'hi') header: 'Connection' put: 'bogus-conn'.
        out := Wire render: r keepAlive: true.
        (Wire in: out has: 'Connection: keep-alive') and: [ (Wire in: out has: 'bogus-conn') not ] ].
    HttpT check: 'render: user Content-Length overridden by real size' that: [
        | r out | r := ((HttpResponse ok) text: 'hi') header: 'Content-Length' put: 999.
        out := Wire render: r keepAlive: true.
        (Wire in: out has: 'Content-Length: 2') and: [ (Wire in: out has: '999') not ] ].
]

"==================== HttpResponse: reading (readFrom:) + round-trip ===================="
[
    HttpT check: 'readFrom: parses status code' equals: 201 is: [
        (Wire readResponse: (Wire message: 'HTTP/1.1 201 Created'
            headers: #('Content-Type: application/json' 'Content-Length: 11') body: '{"ok":true}')) status ].
    HttpT check: 'readFrom: header keys lowercased + case-insensitive lookup' equals: 'application/json' is: [
        (Wire readResponse: (Wire message: 'HTTP/1.1 200 OK'
            headers: #('Content-Type: application/json' 'Content-Length: 11') body: '{"ok":true}')) header: 'content-type' ].
    HttpT check: 'readFrom: body captured by content-length' equals: '{"ok":true}' is: [
        (Wire readResponse: (Wire message: 'HTTP/1.1 200 OK'
            headers: #('Content-Length: 11') body: '{"ok":true}')) body ].
    HttpT check: 'readFrom: jsonBody decodes the parsed body' equals: true is: [
        (Wire readResponse: (Wire message: 'HTTP/1.1 200 OK'
            headers: #('Content-Length: 11') body: '{"ok":true}')) jsonBody at: 'ok' ].
    HttpT check: 'round-trip status survives render then read' equals: 200 is: [
        | built wire | built := (HttpResponse ok) json: (Dictionary new at: 'k' put: 'v'; yourself).
        wire := Wire render: built keepAlive: false. (Wire readResponse: wire) status ].
    HttpT check: 'round-trip body survives render then read' equals: 'v' is: [
        | built wire | built := (HttpResponse ok) json: (Dictionary new at: 'k' put: 'v'; yourself).
        wire := Wire render: built keepAlive: false. (Wire readResponse: wire) jsonBody at: 'k' ].
    HttpT check: 'round-trip connection header survives render then read' equals: 'close' is: [
        | built wire | built := (HttpResponse ok) json: (Dictionary new at: 'k' put: 'v'; yourself).
        wire := Wire render: built keepAlive: false. (Wire readResponse: wire) header: 'connection' ].
]

"==================== HttpRouter ===================="
[
    HttpT check: 'router selects the GET handler and returns its value' equals: 'health-ok' is: [
        | router | router := HttpRouter new.
        router get: '/health' do: [:req | 'health-ok' ].
        router dispatch: (HttpRequest new setMethod: 'GET' target: '/health' version: 'HTTP/1.1') ].
    HttpT check: 'router binds a :id path param' equals: '42' is: [
        | router | router := HttpRouter new.
        router get: '/cards/:id' do: [:req | req param: 'id' ].
        router dispatch: (HttpRequest new setMethod: 'GET' target: '/cards/42' version: 'HTTP/1.1') ].
    HttpT check: 'router binds multiple path params' equals: 'u7-p9' is: [
        | router | router := HttpRouter new.
        router get: '/users/:uid/posts/:pid' do: [:req | 'u', (req param: 'uid'), '-p', (req param: 'pid') ].
        router dispatch: (HttpRequest new setMethod: 'GET' target: '/users/7/posts/9' version: 'HTTP/1.1') ].
    HttpT check: 'router param coexists with query string' equals: '5' is: [
        | router | router := HttpRouter new.
        router get: '/cards/:id' do: [:req | req param: 'id' ].
        router dispatch: (HttpRequest new setMethod: 'GET' target: '/cards/5?x=1' version: 'HTTP/1.1') ].
    HttpT check: 'router root path matches' equals: 'root' is: [
        | router | router := HttpRouter new.
        router get: '/' do: [:req | 'root' ].
        router dispatch: (HttpRequest new setMethod: 'GET' target: '/' version: 'HTTP/1.1') ].
    HttpT check: 'router unknown path answers 404' equals: 404 is: [
        | router | router := HttpRouter new.
        router get: '/known' do: [:req | 'x' ].
        (router dispatch: (HttpRequest new setMethod: 'GET' target: '/nope' version: 'HTTP/1.1')) status ].
    HttpT check: 'router known path wrong method answers 405' equals: 405 is: [
        | router | router := HttpRouter new.
        router get: '/only' do: [:req | 'x' ].
        (router dispatch: (HttpRequest new setMethod: 'DELETE' target: '/only' version: 'HTTP/1.1')) status ].
    HttpT check: 'router dispatches POST and GET on the same path separately' equals: 'posted' is: [
        | router | router := HttpRouter new.
        router get: '/thing' do: [:req | 'got' ].
        router post: '/thing' do: [:req | 'posted' ].
        router dispatch: (HttpRequest new setMethod: 'POST' target: '/thing' version: 'HTTP/1.1') ].
    HttpT check: 'router picks the method-matching route among several' equals: 'the-put' is: [
        | router | router := HttpRouter new.
        router get: '/r' do: [:req | 'the-get' ].
        router put: '/r' do: [:req | 'the-put' ].
        router delete: '/r' do: [:req | 'the-delete' ].
        router dispatch: (HttpRequest new setMethod: 'PUT' target: '/r' version: 'HTTP/1.1') ].
    HttpT check: 'router does not match a shorter path against a longer pattern' equals: 404 is: [
        | router | router := HttpRouter new.
        router get: '/cards/:id' do: [:req | 'x' ].
        (router dispatch: (HttpRequest new setMethod: 'GET' target: '/cards' version: 'HTTP/1.1')) status ].
    HttpT check: 'router literal segment must match exactly' equals: 404 is: [
        | router | router := HttpRouter new.
        router get: '/a/b' do: [:req | 'x' ].
        (router dispatch: (HttpRequest new setMethod: 'GET' target: '/a/c' version: 'HTTP/1.1')) status ].
    ^HttpT report
]
