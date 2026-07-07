"Generic multicore substrate test (NO actors): the running program spawns a
 WORKER isolate on another core with `Isolate spawn:` and exchanges values with
 it — ONE application spread across cores, not a second process. Also guards that
 SmallInteger 0 survives the cross-isolate copy as 0 (tagInt(0)=0 must not be
 mistaken for the deserializer's error sentinel)."
[
	| worker reply |
	"the worker receives a String then a 0, verifies the 0, and replies the
	 String's size back to the main isolate (id 0)."
	worker := Isolate spawn: '
		[ | greeting zero |
		  greeting := Isolate receive.
		  zero := Isolate receive.
		  (zero isNil or: [ zero ~= 0 ])
		      ifTrue: [ [ Isolate send: -1 to: 0 ] whileFalse: [ (Delay forMilliseconds: 2) wait ] ]
		      ifFalse: [ [ Isolate send: greeting size to: 0 ] whileFalse: [ (Delay forMilliseconds: 2) wait ] ] ]
	'.
	Assert true: worker > 0.
	[ Isolate send: 'hello from main' to: worker ] whileFalse: [ (Delay forMilliseconds: 2) wait ].
	[ Isolate send: 0 to: worker ] whileFalse: [ (Delay forMilliseconds: 2) wait ].
	reply := Isolate receive.
	Transcript nextPutAll: 'main got reply from worker: '; nextPutAll: reply printString; lf.
	Assert true: reply = 15.
]
