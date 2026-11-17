# VBT tables: methods and implementation

User reference: [VBT tables](../../user/reference/vbt-viewer.md).

## Reading in place

Nothing is converted. The backend reads the columns of a point table, and each
field of a volume table, where they lie in the `.bin`, at their offset in the
file, so opening a 512³ field costs no copy and no disk space. When a point
table has more rows than a page (100 000), the window loads an even sample of
them rather than its first rows, which in a snapshot are a few blocks of space;
*Load more…* fills in the rows between those loaded.

Either half of the pair can be chosen: the backend resolves a chosen binary to
the header beside it. A binary whose header is missing is refused, since the
bytes alone carry no description.

Splat intensity applies with or without a colour column (issue #263): with
*Single colour*, every intensity used to draw the same, fully opaque picture.

## Byte-order detection

Some converters write a table's values in one byte order and its `.head` in
the other. Read as the header says, every value is absurd: masses of -1e24 on
every row, positions from 1e-26 to 1e30, and a ray-traced view that stays
black.

When a table (points or a volume) opens, the backend reads a few thousand
values of each field, spread over the file, and looks at them both ways. If
they are absurd read as declared and plausible read the other way, the client
asks. The dialog shows the share of absurd values each way and the first value
of the first field read both ways.

- The suggested order is the default button when the evidence is
  overwhelming (at least 40 % of the sampled values absurd as declared, at
  most one in a hundred the other way, over two or more fields), or when it
  was chosen before. Otherwise the header stays the default. *Escape* answers
  nothing: the table opens as it is read now, and nothing is remembered.
- The answer is kept by the backend for that file (its `.head` and data as
  they are), in every window and for every user of that backend, until it is
  changed or the `.head` is corrected. Its other fields and a reconnect after a
  session expiry read it the same way without asking. Opening the file again
  (a new window, *Recent*) asks again, so a choice can be undone. Windows
  already showing the file in the other order reconnect and read it the new
  way.
- An exported recipe records the order the backend read the dataset in as
  its `byte_order` (and the header's as `byte_order_declared`).
- Large physical values are not mistaken for a wrong byte order: the check
  compares each field with its own typical magnitude, so cgs masses (1e33 g)
  or densities (1e-24 g/cm³) are fine.
- A table whose fields are all constant cannot be judged, and opens without
  a question.

A big-endian file read as little-endian produces finite, plausible-looking
values rather than an error, which is why the check exists.
