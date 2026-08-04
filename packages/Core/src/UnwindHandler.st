"One pending ensure:/ifCurtailed: registration, linked per fiber (see
 Block>>valueUnwindProtected:). The unwinders (exception, non-local return,
 Process terminate) run and unlink these; the VM walks the chain from C."

UnwindHandler := Object [

	"An ordinary object until ensure: exists in jit-v2, for the same reason as
	 ExceptionHandler: the chain the VM walks is the unwind record chain the
	 non-local return built, not a chain of these."


	| parent context block |

]
