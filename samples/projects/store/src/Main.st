"A business day at the coffee store: every layer is a namespace, every
 object crosses at least one namespace boundary, and the day closes with
 three fibers re-computing the revenue in parallel and agreeing with the
 sequential number."
Main := Object [

	class main: args [
		| catalog inventory orders bus builder invoices report |
		catalog := self buildCatalog.
		inventory := self stockFor: catalog.
		orders := (Storage at: #Repository) new.
		bus := (Events at: #EventBus) new.

		bus on: #orderPlaced do: [ :order |
			Transcript nextPutAll: 'placed  #'.
			Transcript nextPutAll: order id printString.
			Transcript nextPutAll: ' '; nextPutAll: order customer name.
			Transcript nextPutAll: ' ('; nextPutAll: order itemCount printString.
			Transcript nextPutAll: ' items, '; nextPutAll: order total printString.
			Transcript nextPutAll: ')'; lf ].
		bus on: #orderRejected do: [ :error |
			Transcript nextPutAll: 'REJECTED '; nextPutAll: error messageText; lf ].

		self banner: 'catalog'.
		catalog do: [ :product |
			Transcript nextPutAll: '  '; nextPutAll: product printString; lf ].

		self banner: 'orders'.
		(self ordersOver: catalog) do: [ :order |
			self place: order into: orders taking: inventory notifying: bus ].

		self banner: 'invoices'.
		builder := (Billing at: #InvoiceBuilder) new.
		invoices := OrderedCollection new.
		(orders values asSortedCollection: [ :a :b | a id < b id ])
			do: [ :order | invoices add: (builder buildFor: order) ].
		invoices do: [ :invoice |
			Transcript nextPutAll: '  '; nextPutAll: invoice printString; lf ].

		self banner: 'report'.
		report := (Reports at: #SalesReport) over: orders.
		Transcript nextPutAll: 'revenue by category'; lf.
		report printRevenueTableOn: Transcript.
		Transcript nextPutAll: 'best sellers'; lf.
		report printBestSellersOn: Transcript.
		Transcript nextPutAll: 'grand total (pre-tax)  '.
		Transcript nextPutAll: report grandTotal printString; lf.
		Transcript nextPutAll: 'billed with tax        '.
		Transcript nextPutAll: (invoices inject: 0 cents
			into: [ :sum :invoice | sum + invoice total ]) printString; lf.

		self crossCheck: orders against: report grandTotal.
		^0
	]

	class buildCatalog [
		| productClass catalog |
		productClass := Domain at: #Product.
		catalog := OrderedCollection new.
		catalog
			add: (productClass sku: 'SKU-beans' name: 'espresso beans'
				category: #standard price: 1990 cents);
			add: (productClass sku: 'SKU-cro' name: 'butter croissant'
				category: #food price: 850 cents);
			add: (productClass sku: 'SKU-grind' name: 'gold grinder'
				category: #luxury price: 24900 cents);
			add: (productClass sku: 'SKU-mug' name: 'stone mug'
				category: #standard price: 3200 cents).
		^catalog
	]

	class stockFor: aCatalog [
		| inventory quantities |
		inventory := (Storage at: #Inventory) new.
		quantities := Dictionary new.
		quantities at: 'SKU-beans' put: 10.
		quantities at: 'SKU-cro' put: 12.
		quantities at: 'SKU-grind' put: 2.
		quantities at: 'SKU-mug' put: 5.
		aCatalog do: [ :product |
			inventory stock: product sku with: (quantities at: product sku) ].
		^inventory
	]

	class ordersOver: aCatalog [
		| orderClass customerClass beans croissant grinder mug out |
		orderClass := Domain at: #Order.
		customerClass := Domain at: #Customer.
		beans := aCatalog at: 1.
		croissant := aCatalog at: 2.
		grinder := aCatalog at: 3.
		mug := aCatalog at: 4.
		out := OrderedCollection new.
		out add: ((orderClass id: 1 customer: (customerClass named: 'Alice'))
			add: beans quantity: 2; add: croissant quantity: 1; yourself).
		out add: ((orderClass id: 2 customer: (customerClass named: 'Bruno'))
			add: grinder quantity: 1; add: mug quantity: 2; yourself).
		out add: ((orderClass id: 3 customer: (customerClass named: 'Clara'))
			add: grinder quantity: 6; yourself).
		out add: ((orderClass id: 4 customer: (customerClass named: 'Diego'))
			add: croissant quantity: 3; add: beans quantity: 1; yourself).
		^out
	]

	class place: anOrder into: aRepository taking: anInventory notifying: aBus [
		"the handler class is FETCHED from the Storage namespace: on:do:
		 takes the class object, wherever it lives"
		[
			anOrder lines do: [ :line |
				anInventory reserve: line product sku quantity: line quantity ].
			aRepository at: anOrder id put: anOrder.
			aBus publish: #orderPlaced with: anOrder.
		] on: (Storage at: #OutOfStockError) do: [ :error |
			aBus publish: #orderRejected with: error ].
	]

	class crossCheck: anOrderRepository against: aMoney [
		"three fibers each total a slice of the orders and hand the cents back
		 over a channel; the joined sum must agree with the sequential total"
		| all channel parallelCents |
		all := anOrderRepository values.
		channel := Channel new.
		0 to: 2 do: [ :slice |
			[ | mine |
				mine := 0 cents.
				all do: [ :order |
					order id \\ 3 = slice ifTrue: [mine := mine + order total] ].
				channel send: mine cents ] fork ].
		parallelCents := 0.
		1 to: 3 do: [ :i | parallelCents := parallelCents + channel receive ].
		Transcript nextPutAll: 'parallel cross-check   '.
		(aMoney agreesWith: ((Domain at: #Money) cents: parallelCents))
			ifTrue: [Transcript nextPutAll: parallelCents printString.
				Transcript nextPutAll: ' cents from 3 fibers, totals agree'; lf]
			ifFalse: [Transcript nextPutAll: 'MISMATCH'; lf].
	]

	class banner: aString [
		Transcript lf.
		Transcript nextPutAll: '== '; nextPutAll: aString; nextPutAll: ' =='; lf.
	]

]
