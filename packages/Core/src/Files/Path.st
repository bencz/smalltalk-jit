"Path: pure manipulation of POSIX path Strings (the '/' separator). No
 filesystem access whatsoever; File and Directory do the I/O. Class side only."

Path := Object [

	class separator [
		^$/
	]


	class join: aString with: bString [
		"Join two components with exactly one '/'. A trailing '/' on the left or a
		 leading '/' on the right is not duplicated; an empty component yields the
		 other unchanged."
		aString isEmpty ifTrue: [^bString].
		bString isEmpty ifTrue: [^aString].
		(self endsWithSeparator: aString)
			ifTrue: [
				^(self startsWithSeparator: bString)
					ifTrue: [aString, (bString copyFrom: 2 to: bString size)]
					ifFalse: [aString, bString]].
		^(self startsWithSeparator: bString)
			ifTrue: [aString, bString]
			ifFalse: [aString, '/', bString]
	]


	class basename: aString [
		"The final component (after the last '/'). Answers aString unchanged when
		 there is no separator, and '' when aString ends in '/'."
		| i |

		i := self lastSeparatorIndex: aString.
		i = 0 ifTrue: [^aString].
		^aString copyFrom: i + 1 to: aString size
	]


	class dirname: aString [
		"Everything before the final component: '.' when there is no separator,
		 '/' when the only separator is a leading one."
		| i |

		i := self lastSeparatorIndex: aString.
		i = 0 ifTrue: [^'.'].
		i = 1 ifTrue: [^'/'].
		^aString copyFrom: 1 to: i - 1
	]


	class extension: aString [
		"The filename extension after the last '.' of the basename, or '' when the
		 basename has no '.' or is a dotfile (a leading '.')."
		| base i |

		base := self basename: aString.
		i := self lastDotIndex: base.
		(i = 0 or: [i = 1]) ifTrue: [^''].
		^base copyFrom: i + 1 to: base size
	]


	class isAbsolute: aString [
		"True when aString begins with the separator."
		^aString notEmpty and: [self startsWithSeparator: aString]
	]


	"--- private ---"

	class endsWithSeparator: aString [
		^aString notEmpty and: [(aString at: aString size) = self separator]
	]


	class startsWithSeparator: aString [
		^aString notEmpty and: [(aString at: 1) = self separator]
	]


	class lastSeparatorIndex: aString [
		| sep |

		sep := self separator.
		aString size to: 1 by: -1 do: [ :i | (aString at: i) = sep ifTrue: [^i]].
		^0
	]


	class lastDotIndex: aString [
		aString size to: 1 by: -1 do: [ :i | (aString at: i) = $. ifTrue: [^i]].
		^0
	]

]
