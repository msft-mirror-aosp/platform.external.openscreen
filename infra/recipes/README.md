# Open Screen recipes

This directory contains the [LUCI recipes](https://chromium.googlesource.com/infra/luci/recipes-py/+/main/doc/user_guide.md)
used by Open Screen's standalone builders. They were moved here from
[chromium/tools/build](https://chromium.googlesource.com/chromium/tools/build/+/main/recipes/recipes/openscreen.py).

The recipe repo is configured by
[`//infra/config/recipes.cfg`](../config/recipes.cfg), which pins the
revisions of the recipe repos we depend on (`build`, `depot_tools`, `infra`,
and `recipe_engine`). All of these must agree with the revisions pinned by
`build`; use `recipes.py manual_roll` (or let the recipe autoroller do it)
rather than editing them by hand.

`recipes.py` is a vendored bootstrap script from recipes-py. Do not modify it.

## Running the tests

```sh
# Run the simulation tests.
infra/recipes/recipes.py test run

# Regenerate the expectation files after changing a recipe.
infra/recipes/recipes.py test train

# Lint the recipes.
infra/recipes/recipes.py lint
```

The first invocation bootstraps the dependencies into
`infra/recipes/.recipe_deps/`, which is ignored by git.

## Testing on Builders (Googlers)

The builders run recipes from CIPD recipe bundles built from `refs/heads/main`.
Tryjobs on Gerrit run the recipe from `main`, not the CL under test.

To test recipe changes against actual LUCI builders before landing, Googlers
can use [`led`](http://go/led):

```sh
# Fetch builder definition, inject the local recipe bundle, and launch a test job:
led get-builder "chromium/try:openscreen_linux" \
  | led edit-recipe-bundle \
  | led launch
```

## Deployment

The builders run the recipe from the CIPD recipe bundle
`infra/recipe_bundles/chromium.googlesource.com/openscreen`, which is built
from `refs/heads/main`. This means that tryjobs run the recipe from `main`,
not from the CL under test: recipe changes take effect once they land.

