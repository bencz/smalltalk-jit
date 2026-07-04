"============================================================
  ADVANCED - Mediator Pattern
  ------------------------------------------------------------
  The Mediator centralises communication between objects so
  they never refer to each other directly. Here a ChatRoom
  mediates between Users: a user talks to the room, and the
  room decides who receives the message.

    User  --->  ChatRoom (mediator)  --->  User
                                     --->  User

  This keeps the Users decoupled: adding or removing a user
  changes nothing in the other users.
  ============================================================"

ChatRoom := Object [
	| participants |

	class new [ ^super new init ]
	init [ participants := OrderedCollection new ]

	register: aUser [
		participants add: aUser.
		aUser joined: self.
		^aUser
	]

	userNamed: aName [
		^participants detect: [:u | u name = aName] ifNone: [nil]
	]

	"Direct message from one user to another."
	send: aMessage from: sender to: recipientName [
		| recipient |
		recipient := self userNamed: recipientName.
		recipient isNil
			ifTrue:  [Transcript nextPutAll: '  (no such user: '; nextPutAll: recipientName; nextPutAll: ')'; lf]
			ifFalse: [recipient receive: aMessage from: sender name]
	]

	"Message to everyone except the sender."
	broadcastFrom: sender message: aMessage [
		participants do: [:user |
			user == sender ifFalse: [user receive: aMessage from: sender name]]
	]
]

User := Object [
	| name room |

	class named: aName [ ^self new setName: aName ]
	setName: aName [ name := aName ]
	name [ ^name ]
	joined: aRoom [ room := aRoom ]

	say: aMessage to: recipientName [
		Transcript nextPutAll: name; nextPutAll: ' -> '; nextPutAll: recipientName;
			nextPutAll: ': '; nextPutAll: aMessage; lf.
		room send: aMessage from: self to: recipientName.
	]

	shout: aMessage [
		Transcript nextPutAll: name; nextPutAll: ' -> everyone: '; nextPutAll: aMessage; lf.
		room broadcastFrom: self message: aMessage.
	]

	receive: aMessage from: senderName [
		Transcript nextPutAll: '    ['; nextPutAll: name; nextPutAll: ' hears '; nextPutAll: senderName;
			nextPutAll: ': "'; nextPutAll: aMessage; nextPutAll: '"]'; lf.
	]
]

[
	| room alice bob carol |
	room := ChatRoom new.
	alice := room register: (User named: 'Alice').
	bob   := room register: (User named: 'Bob').
	carol := room register: (User named: 'Carol').

	alice say: 'Hi Bob, got a minute?' to: 'Bob'.
	bob   say: 'Sure, Alice!' to: 'Alice'.
	carol shout: 'Lunch in 5, everyone!'.
	alice say: 'ping' to: 'Dave'.
]
