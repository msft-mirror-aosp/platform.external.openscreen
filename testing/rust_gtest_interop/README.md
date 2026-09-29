# Rust GoogleTest Interop

This directory contains integration glue to write Rust unit tests that execute
within `openscreen_unittests` alongside C++ GoogleTest suites.

## Origin and Upstream Tracking

This implementation is vendored from Chromium's `//testing/rust_gtest_interop`.

Currently, standalone repositories using the Chromium build system must vendor
this glue code directly. A generic reuse mechanism across Chromium and
standalone repositories is tracked upstream in
[crbug.com/559867579](https://crbug.com/559867579). Once available, this
vendored copy can be replaced with the upstream shared package.

## Usage

In Rust code:

```rust
use rust_gtest_interop::prelude::*;

#[gtest(MyTestSuite, MyTestCase)]
fn test_something() {
    expect_eq!(2 + 2, 4);
    expect_true!(true);
}
```

In `BUILD.gn`:

```gn
openscreen_rust_static_library("my_rust_tests") {
  testonly = true
  sources = [ "my_test.rs" ]
  deps = [
    "//testing/rust_gtest_interop",
  ]
}
```

And add `:my_rust_tests` to `deps` of `openscreen_unittests`.
