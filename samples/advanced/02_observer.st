"============================================================
  ADVANCED - Observer Pattern
  ------------------------------------------------------------
  A Subject maintains a list of Observers and notifies them
  whenever its state changes. The observers react without the
  subject knowing their concrete types.

  Here a WeatherStation (subject) pushes temperature readings
  to several displays (observers).
  ============================================================"

Subject := Object [
	| observers |

	class new [ ^super new initSubject ]
	initSubject [ observers := OrderedCollection new ]

	addObserver: anObserver [ observers add: anObserver. ^anObserver ]
	removeObserver: anObserver [ observers remove: anObserver ifAbsent: [] ]

	changed: anEvent [
		observers do: [:each | each update: anEvent from: self]
	]
]

WeatherStation := Subject [
	| temperature |

	temperature [ ^temperature ]

	temperature: aNumber [
		temperature := aNumber.
		self changed: aNumber.        "notify all observers"
	]
]

"Each observer implements update:from:."
CurrentDisplay := Object [
	update: reading from: station [
		Transcript nextPutAll: '  [Current]  now ';
			nextPutAll: reading printString; nextPutAll: ' C'; lf.
	]
]

AlertDisplay := Object [
	| threshold |
	class threshold: n [ ^self new setThreshold: n ]
	setThreshold: n [ threshold := n ]

	update: reading from: station [
		reading > threshold ifTrue: [
			Transcript nextPutAll: '  [Alert!]   ';
				nextPutAll: reading printString;
				nextPutAll: ' C is above '; nextPutAll: threshold printString; lf].
	]
]

MinMaxDisplay := Object [
	| low high |
	class new [ ^super new initMinMax ]
	initMinMax [ low := nil. high := nil ]

	update: reading from: station [
		low  := low  isNil ifTrue: [reading] ifFalse: [low min: reading].
		high := high isNil ifTrue: [reading] ifFalse: [high max: reading].
		Transcript nextPutAll: '  [MinMax]   low ';
			nextPutAll: low printString; nextPutAll: ', high '; nextPutAll: high printString; lf.
	]
]

[
	| station |
	station := WeatherStation new.
	station addObserver: CurrentDisplay new.
	station addObserver: (AlertDisplay threshold: 30).
	station addObserver: MinMaxDisplay new.

	#(22 28 35 19 31) do: [:reading |
		Transcript nextPutAll: 'reading: '; nextPutAll: reading printString; lf.
		station temperature: reading].
]
