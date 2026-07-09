[
	"Socket connect:/bind: must raise a catchable error on wrong-typed arguments,
	 not crash the VM. Regression for socketConnectPrimitive/socketBindPrimitive,
	 which used to asObject/asCInt the address/port with no type check — a String,
	 a bare Object (too small for the address field), or an InternetAddress whose
	 address ivar is still nil would read out of bounds and abort the process.
	 These cases fail type validation before any syscall, so they need no network."
	Assert do: [Socket connect: 'not an address' port: 1] expect: Error.
	Assert do: [Socket connect: Object new port: 1] expect: Error.
	Assert do: [Socket connect: InternetAddress new port: 1] expect: Error.
	Assert do: [Socket connect: (InternetAddress lookup: '127.0.0.1') port: 'x'] expect: Error.

	Assert do: [ServerSocket bind: 'not an address' port: 1 queueSize: 1] expect: Error.
	Assert do: [ServerSocket bind: Object new port: 1 queueSize: 1] expect: Error.
]
