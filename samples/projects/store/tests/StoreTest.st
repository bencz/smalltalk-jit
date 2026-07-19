[ | t money productClass orderClass customerClass beans grinder order builder invoice inventory bus seen |
	t := TestRun named: 'Store'.

	"Money value object (Domain), via the Integer extension and the class"
	money := 1990 cents.
	t check: 'the cents extension answers Money' equals: 1990 is: [ money cents ].
	t check: 'money arithmetic' equals: 4830 is: [ ((money * 2) + 850 cents) cents ].
	t check: 'percent rounds to the cent' equals: 398 is: [ (3980 cents percent: 10) cents ].
	t check: 'money prints like money' equals: '$ 19.90' is: [ money printString ].
	t check: 'and pads its cents' equals: '$ 3.05' is: [ 305 cents printString ].

	"an order made of Domain instances"
	productClass := Domain at: #Product.
	orderClass := Domain at: #Order.
	customerClass := Domain at: #Customer.
	beans := productClass sku: 'B' name: 'beans' category: #standard price: 1990 cents.
	grinder := productClass sku: 'G' name: 'grinder' category: #luxury price: 24900 cents.
	order := (orderClass id: 7 customer: (customerClass named: 'Test'))
		add: beans quantity: 2; add: grinder quantity: 1; yourself.
	t check: 'order totals through its lines' equals: 28880 is: [ order total cents ].
	t check: 'order counts items' equals: 3 is: [ order itemCount ].

	"tax polymorphism (Billing) over the Domain order"
	builder := (Billing at: #InvoiceBuilder) new.
	invoice := builder buildFor: order.
	t check: 'per-line tax by category (10% + 25%)'
		equals: 398 + 6225 is: [ invoice tax cents ].
	t check: 'invoice total adds the tax'
		equals: 28880 + 6623 is: [ invoice total cents ].

	"inventory guard raises the Storage exception, caught by FETCHED class"
	inventory := (Storage at: #Inventory) new.
	inventory stock: 'G' with: 1.
	t expect: 'over-reserving raises OutOfStockError'
		raises: (Storage at: #OutOfStockError)
		in: [ inventory reserve: 'G' quantity: 3 ].
	t check: 'a failed reservation leaves stock untouched'
		equals: 1 is: [ inventory availableOf: 'G' ].
	inventory reserve: 'G' quantity: 1.
	t check: 'a good reservation decrements' equals: 0 is: [ inventory availableOf: 'G' ].

	"the event bus delivers instances to block handlers"
	bus := (Events at: #EventBus) new.
	seen := OrderedCollection new.
	bus on: #ping do: [ :payload | seen add: payload ].
	bus on: #ping do: [ :payload | seen add: payload ].
	t check: 'publish answers the handler count' equals: 2 is: [ bus publish: #ping with: order ].
	t check: 'handlers received the very same instance'
		that: [ (seen at: 1) == order and: [ (seen at: 2) == order ] ].
	t check: 'unknown topics deliver to nobody' equals: 0 is: [ bus publish: #nope with: 1 ].

	^t report
]
