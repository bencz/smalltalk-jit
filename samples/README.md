# Smalltalk-JIT Samples

A collection of small, self-contained programs that show what the VM can do —
from "hello world" up to design patterns and a Mandelbrot renderer.

Every file here has been run end-to-end against a freshly bootstrapped image.

## Running a sample

Build the VM and bootstrap an image once:

```sh
cmake -S . -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j4
export LD_LIBRARY_PATH=$PWD/build
./build/st -s snapshot -b smalltalk        # writes ./snapshot
```

Then run any sample with `-f`:

```sh
./build/st -s snapshot -f samples/01_hello_world.st
./build/st -s snapshot -f samples/advanced/01_mediator.st
```

Run them all:

```sh
for f in samples/*.st samples/advanced/*.st; do
    echo "== $f =="; ./build/st -s snapshot -f "$f"
done
```

## The basics (`samples/`)

| File | Shows |
|------|-------|
| `01_hello_world.st`   | Transcript output, strings, cascades |
| `02_numbers.st`       | The numeric tower: SmallInteger, LargeInteger, Fraction, Float, radices |
| `03_control_flow.st`  | Conditionals, loops and boolean logic (all just messages) |
| `04_blocks_closures.st` | Blocks as first-class objects, closures, higher-order blocks |
| `05_collections.st`   | Array, OrderedCollection, Set, Bag, Dictionary, Interval + enumeration |
| `06_strings.st`       | Characters, case, reversing, palindromes, splitting |
| `07_classes.st`       | Class definitions, inheritance, polymorphism, custom `printOn:` |
| `08_exceptions.st`    | `on:do:`, `ensure:`, `return:`, `retry`, custom exception classes |
| `09_recursion.st`     | factorial, Fibonacci, gcd, digit sum, Towers of Hanoi |
| `10_fizzbuzz.st`      | The interview classic |
| `11_sieve_primes.st`  | Sieve of Eratosthenes |
| `12_sorting.st`       | Hand-written bubble sort & quicksort, plus `SortedCollection` |
| `13_bank_account.st`  | A domain model with a custom exception |
| `14_linked_list.st`   | A `Collection` subclass built from cons cells (gets enumeration for free) |
| `15_stack_queue.st`   | LIFO stack and FIFO queue on top of OrderedCollection |
| `16_temperature.st`   | Exact Fraction arithmetic vs. Float display |
| `17_mandelbrot.st`    | ASCII Mandelbrot via fixed-point integer math |
| `18_word_frequency.st`| Tallying words in a Dictionary, sorting by frequency |
| `19_roman_numerals.st`| Greedy conversion driven by `->` associations |
| `20_rpn_calculator.st`| Evaluating Reverse Polish Notation with an operand stack |

## Design patterns & bigger programs (`samples/advanced/`)

| File | Pattern / program |
|------|-------------------|
| `01_mediator.st`                | **Mediator** — a chat room coordinating decoupled users |
| `02_observer.st`                | **Observer** — a weather station pushing readings to displays |
| `03_strategy.st`                | **Strategy** — pluggable discount algorithms (blocks & objects) |
| `04_state.st`                   | **State** — a turnstile finite-state machine |
| `05_composite.st`               | **Composite** — a file-system tree that sums its own sizes |
| `06_visitor.st`                 | **Visitor** — evaluate and pretty-print an expression AST |
| `07_command.st`                 | **Command** — a text editor with undo |
| `08_decorator.st`               | **Decorator** — a coffee shop stacking condiments |
| `09_chain_of_responsibility.st` | **Chain of Responsibility** — a support-desk escalation chain |
| `10_game_of_life.st`            | Conway's **Game of Life** — a glider walking the grid |
| `11_task_scheduler.st`          | A cooperative **task scheduler** driven purely by polymorphic dispatch: the pattern `benchmarks/Richards.st` measures |

## Concurrency & networking (`samples/concurrency/`)

Cooperative green threads (fibers) on a single OS thread, with an epoll-backed
scheduler, channels, an actor layer, and non-blocking sockets.

| File | Shows |
|------|-------|
| `01_channels.st`            | **Channels** — a worker pool and a producer→filter→printer pipeline |
| `02_actors_intro.st`        | **Actors** — a bank-account actor and "let it crash" supervision |
| `03_user_registry.st`       | **Actors** — a registry that spawns/supervises one actor per user |
| `04_webserver.st`           | Non-blocking **sockets** — a tiny concurrent HTTP server + clients |
| `05_business_card_api.st`   | **HTTP + actors** — a business-card API (users and cards are actors, each card counts its views), driven by an in-VM client with a throughput burst |
| `06_business_card_server.st`| The same API as a **standalone server** bound to `0.0.0.0:8080`, seeded with one card; hammer it with `curl`, `ab -k -n 50000 -c 200`, or `wrk` |
| `07_mediator_cqrs.st`       | **Advanced Mediator (CQRS)** — commands/queries routed to one handler through a pipeline (`log → validate → time`); events published **fire-and-forget** to per-subscriber fibers; a saga reacts to a `StockLow` event by issuing a `Restock`; concurrent buyers fan out as fibers and fan in over a channel |

The HTTP server, client and JSON codec used by 05/06 live in the kernel
(`smalltalk/Http/*.st` and `smalltalk/Json.st`), so they are reusable
building blocks, not just sample code:

```smalltalk
server := HttpServer port: 8080.
server get: '/cards/:id' do: [:req | (HttpResponse ok) json: aDictionary ].
server start.

client := HttpClient on: (InternetAddress lookup: '127.0.0.1') port: 8080.
(client get: '/cards/1') jsonBody printNl.
```

## Notes for writing your own samples

A few things specific to this VM that are worth knowing:

- **Class definition** uses the assignment form:
  ```smalltalk
  Point := Object [
      | x y |
      class x: ax y: ay [ ^self new setX: ax y: ay ]
      setX: ax y: ay [ x := ax. y := ay ]
      + other [ ^Point x: x + other x y: y + other y ]
  ]
  ```
- **Top-level code** goes in `[ ... ]` blocks, evaluated as the file loads.
- **Temporaries** (`| a b |`) must be declared at the very start of a method or block.
- `printNl` prints any object; `Transcript nextPutAll:` writes a String, `lf` a newline.
- Negative numbers are not allowed inside a `#( ... )` literal array — build such
  lists with an `OrderedCollection` instead.
- For tight numeric loops prefer integer arithmetic (see `17_mandelbrot.st`); the
  integer tower (including arbitrary-precision LargeInteger) is fast and exact.
