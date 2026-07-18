"=== Test-only helper: structural deep equality for parsed JSON (Dictionary /
 OrderedCollection / leaves). Objects are unordered, so Dictionaries are compared
 key by key, arrays index by index. Used to assert that parse -> render -> parse
 reproduces an equal structure.

 NOTE ON STRUCTURE: the VM caps each block/method scope at 256 native handles
 (vm/core/Handle.h), so a single driver block with ~110+ statements aborts. Like
 the existing JsonTest.st, the checks are split across several top-level blocks
 that share ONE TestRun via the global JsonT; the last block ends with ^JsonT
 report. NO emoji, NO em-dash (hard project rule). ==="
JDeep := Object [
    class eq: x with: y [
        (x isKindOf: Dictionary) ifTrue: [
            (y isKindOf: Dictionary) ifFalse: [ ^false ].
            x size = y size ifFalse: [ ^false ].
            x keysAndValuesDo: [:k :v |
                (self eq: v with: (y at: k ifAbsent: [ #jsonExtraMissing ])) ifFalse: [ ^false ] ].
            ^true ].
        (x isKindOf: OrderedCollection) ifTrue: [
            (y isKindOf: OrderedCollection) ifFalse: [ ^false ].
            x size = y size ifFalse: [ ^false ].
            1 to: x size do: [:i | (self eq: (x at: i) with: (y at: i)) ifFalse: [ ^false ] ].
            ^true ].
        ^x = y
    ]
    "parse -> encode -> parse and confirm the two structures match"
    class roundTrips: aJsonString [
        | p1 p2 |
        p1 := Json parse: aJsonString.
        p2 := Json parse: (Json encode: p1).
        ^self eq: p1 with: p2
    ]
    "Build the expected escaped forms WITHOUT a source backslash (codePoint 92),
     so the test file needs no literal backslash-u sequences."
    class bs [ | s | s := String new: 1. s at: 1 put: (Character codePoint: 92). ^s ]
    class u: hex4 [ ^(String with: $") , self bs , 'u' , hex4 , (String with: $") ]
]

"==================== string escape DECODING (beyond the JsonTest cases) ===================="
[
    JsonT := TestRun named: 'JsonExtra'.

    JsonT check: 'decode escape: carriage return' equals: 13 is: [ (Json parse: '"\r"') first codePoint ].
    JsonT check: 'decode escape: backspace' equals: 8 is: [ (Json parse: '"\b"') first codePoint ].
    JsonT check: 'decode escape: form feed' equals: 12 is: [ (Json parse: '"\f"') first codePoint ].
    JsonT check: 'decode escape: literal backslash' equals: 92 is: [ (Json parse: '"\\"') first codePoint ].
    JsonT check: 'decode escape: forward slash' equals: '/' is: [ Json parse: '"\/"' ].
    JsonT check: 'decode mixed escapes preserves length' equals: 4 is: [ (Json parse: '"a\rb\f"') size ].
    JsonT check: 'decode empty string' equals: 0 is: [ (Json parse: '""') size ].
    JsonT check: 'empty object parses to an empty Dictionary' that: [
        | d | d := Json parse: '{}'. (d isKindOf: Dictionary) and: [ d isEmpty ] ].
    JsonT check: 'empty array parses to an empty OrderedCollection' that: [
        | a | a := Json parse: '[]'. (a isKindOf: OrderedCollection) and: [ a isEmpty ] ].
    JsonT check: 'nested empty containers: array field is empty' that: [
        | d | d := Json parse: '{"a":[],"b":{}}'. (d at: 'a') isEmpty ].
    JsonT check: 'nested empty containers: object field is empty' that: [
        | d | d := Json parse: '{"a":[],"b":{}}'. (d at: 'b') isEmpty ].
    JsonT check: 'encode empty string' equals: '""' is: [ Json encode: '' ].
    JsonT check: 'encode does not escape forward slash' equals: '"a/b"' is: [ Json encode: 'a/b' ].
]

"==================== control-character ENCODING (beyond JsonTest \n) ===================="
[
    JsonT check: 'encode tab as backslash-t' equals: '"\t"' is: [
        | s | s := String new: 1. s at: 1 put: Character tab. Json encode: s ].
    JsonT check: 'encode carriage return as backslash-r' equals: '"\r"' is: [
        | s | s := String new: 1. s at: 1 put: Character cr. Json encode: s ].
    JsonT check: 'encode backspace as backslash-b' equals: '"\b"' is: [
        | s | s := String new: 1. s at: 1 put: (Character codePoint: 8). Json encode: s ].
    JsonT check: 'encode form feed as backslash-f' equals: '"\f"' is: [
        | s | s := String new: 1. s at: 1 put: (Character codePoint: 12). Json encode: s ].
    JsonT check: 'encode control 0x01 as a unicode escape' equals: (JDeep u: '0001') is: [
        | s | s := String new: 1. s at: 1 put: (Character codePoint: 1). Json encode: s ].
    JsonT check: 'encode control 0x07 as a unicode escape' equals: (JDeep u: '0007') is: [
        | s | s := String new: 1. s at: 1 put: (Character codePoint: 7). Json encode: s ].
    JsonT check: 'encode control 0x1B with uppercase hex digits' equals: (JDeep u: '001B') is: [
        | s | s := String new: 1. s at: 1 put: (Character codePoint: 27). Json encode: s ].
    JsonT check: 'encode control 0x1F with uppercase hex digits' equals: (JDeep u: '001F') is: [
        | s | s := String new: 1. s at: 1 put: (Character codePoint: 31). Json encode: s ].
    JsonT check: 'encoded control char round-trips back to its code point' equals: 27 is: [
        | s back | s := String new: 1. s at: 1 put: (Character codePoint: 27).
        back := Json parse: (Json encode: s). back first codePoint ].
]

"==================== number TYPES (JsonTest checks values, not classes) ===================="
[
    JsonT check: 'integer parses to an Integer' that: [ (Json parse: '5') isKindOf: Integer ].
    JsonT check: 'integer is not a Float' that: [ ((Json parse: '5') isKindOf: Float) not ].
    JsonT check: 'fractional literal parses to a Float' that: [ (Json parse: '1.5') isKindOf: Float ].
    JsonT check: 'exponent literal parses to a Float' that: [ (Json parse: '1e2') isKindOf: Float ].
    JsonT check: 'exponent literal has the right value' equals: 100 is: [ Json parse: '1e2' ].
    JsonT check: 'exact exponent float value' equals: 125.0 is: [ Json parse: '1.25e2' ].
    JsonT check: 'negative integer value' equals: 7 is: [ (Json parse: '-7') negated ].
    JsonT check: 'negative integer stays an Integer' that: [ (Json parse: '-7') isKindOf: Integer ].
    JsonT check: 'negative float value' that: [ (Json parse: '-3.14') = 3.14 negated ].
    JsonT check: 'plain zero value' equals: 0 is: [ Json parse: '0' ].
    JsonT check: 'zero is an Integer' that: [ (Json parse: '0') isKindOf: Integer ].
    JsonT check: 'small-exponent float parses' that: [ (Json parse: '2.5e-3') isKindOf: Float ].
]

"==================== whitespace tolerance + value ACCESS ===================="
[
    JsonT check: 'whitespace (tab/newline/space) around tokens is tolerated' equals: 1 is: [
        | out | out := CollectionStream with: (String new: 16).
        out nextPut: Character tab; nextPut: Character lf.
        out nextPutAll: '{ "a" : 1 }'. out nextPut: Character lf.
        (Json parse: out contents) at: 'a' ].
    JsonT check: 'object keys: count' equals: 3 is: [ (Json parse: '{"a":1,"b":2,"c":3}') keys size ].
    JsonT check: 'object keys: membership' that: [
        | k | k := (Json parse: '{"a":1,"b":2,"c":3}') keys. (k includes: 'a') and: [ k includes: 'c' ] ].
    JsonT check: 'object at:ifAbsent: yields the default for a missing key' equals: 'def' is: [
        (Json parse: '{"a":1}') at: 'missing' ifAbsent: [ 'def' ] ].
    JsonT check: 'object at: reads a present key' equals: 2 is: [ (Json parse: '{"a":1,"b":2}') at: 'b' ].
    JsonT check: 'array do: visits every element' equals: 60 is: [
        | sum | sum := 0. (Json parse: '[10,20,30]') do: [:e | sum := sum + e ]. sum ].
    JsonT check: 'array at: reads by index' equals: 20 is: [ (Json parse: '[10,20,30]') at: 2 ].
    JsonT check: 'array size' equals: 3 is: [ (Json parse: '[10,20,30]') size ].
]

"==================== parse -> render -> parse round-trips (structural equality) ===================="
[
    JsonT check: 'round-trip: mixed object with array + nested object + escape' that: [
        JDeep roundTrips: '{"a":1,"b":[1,2,3],"c":{"d":null,"e":true},"f":"x\ny"}' ].
    JsonT check: 'round-trip: heterogeneous array incl empty containers' that: [
        JDeep roundTrips: '[0.5,1e2,-7,"x",[],{},false]' ].
    JsonT check: 'round-trip: three-level nested object' that: [
        JDeep roundTrips: '{"nested":{"deep":{"x":[true,false,null,"end"]}}}' ].
    JsonT check: 'round-trip: OrderedCollection of Dictionaries via encode' that: [
        | oc back | oc := OrderedCollection new.
        oc add: (Dictionary new at: 'k' put: 1; yourself).
        oc add: (Dictionary new at: 'k' put: 2; yourself).
        back := Json parse: (Json encode: oc).
        JDeep eq: oc with: back ].
    JsonT check: 'round-trip: multi-byte UTF-8 payload survives encode+parse' equals: true is: [
        | s | s := Json parse: '"é"'. (Json parse: (Json encode: s)) = s ].
    JsonT check: 'round-trip: string with every named escape' that: [
        JDeep roundTrips: '"tab\tnl\ncr\rbs\bff\fq\"sl\\"' ].
]

"==================== bounded deep nesting (valid, depth 20) ===================="
[
    JsonT check: 'deep nested arrays (depth 20) drill to the innermost value' equals: 0 is: [
        | s p cur | s := '0'. 1 to: 20 do: [:i | s := '[', s, ']' ].
        p := Json parse: s. cur := p. 20 timesRepeat: [ cur := cur at: 1 ]. cur ].
    JsonT check: 'deep nested objects (depth 20) drill to the innermost value' equals: 0 is: [
        | s p cur | s := '0'. 1 to: 20 do: [:i | s := '{"k":', s, '}' ].
        p := Json parse: s. cur := p. 20 timesRepeat: [ cur := cur at: 'k' ]. cur ].
    JsonT check: 'deep nested arrays (depth 20) round-trip' that: [
        | s | s := '0'. 1 to: 20 do: [:i | s := '[', s, ']' ]. JDeep roundTrips: s ].
    JsonT check: 'deep nested objects (depth 20) round-trip' that: [
        | s | s := '0'. 1 to: 20 do: [:i | s := '{"k":', s, '}' ]. JDeep roundTrips: s ].
]

"==================== malformed input (distinct forms) + error hierarchy ===================="
[
    JsonT expect: 'leading comma in array' raises: JsonParseError in: [ Json parse: '[,1]' ].
    JsonT expect: 'double comma in array' raises: JsonParseError in: [ Json parse: '[1,,2]' ].
    JsonT expect: 'comma-only object' raises: JsonParseError in: [ Json parse: '{,}' ].
    JsonT expect: 'bare NaN token' raises: JsonParseError in: [ Json parse: 'NaN' ].
    JsonT expect: 'capitalised True token' raises: JsonParseError in: [ Json parse: 'True' ].
    JsonT expect: 'mismatched array close brace' raises: JsonParseError in: [ Json parse: '[1}' ].
    JsonT expect: 'bare comma' raises: JsonParseError in: [ Json parse: ',' ].
    JsonT expect: 'unterminated empty array' raises: JsonParseError in: [ Json parse: '[' ].
    JsonT expect: 'unicode escape with no hex digits' raises: JsonParseError in: [ Json parse: '"\u"' ].
    JsonT expect: 'trailing closing brace after a value' raises: JsonParseError in: [ Json parse: '{"a":1}}' ].
    JsonT expect: 'two top-level values' raises: JsonParseError in: [ Json parse: '1 2' ].
    JsonT expect: 'JsonParseError is a kind of JsonError' raises: JsonError in: [ Json parse: '{' ].
    JsonT check: 'parse error reports the 1-based position of the fault' equals: 2 is: [
        | p | p := nil.
        [ Json parse: '[,1]' ] on: JsonParseError do: [:e | p := e position ]. p ].

    ^JsonT report
]
