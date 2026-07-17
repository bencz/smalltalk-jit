"One pending ensure:/ifCurtailed: registration, linked per fiber (see
 Block>>valueUnwindProtected:). The unwinders (exception, non-local return,
 Process terminate) run and unlink these; the VM walks the chain from C."

UnwindHandler := Object [

	<shape: UnwindHandlerShape>

	| parent context block |

]
