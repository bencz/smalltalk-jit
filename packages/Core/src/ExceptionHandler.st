ExceptionHandler := Object [

	"An ordinary object until the exception machinery exists in jit-v2. It used
	 to carry a VM shape because the C unwinder walked these directly; the v2
	 unwinder is the one the non-local return already built (jit/Jit.c), and what
	 it walks is a chain of records on the C stack."

	| parent context |

]
