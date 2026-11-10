# zuxlsx's other LinkingTo siblings (zuxml, zucrypt), from their own main
# branches, for tools/ci/zuxlsx.sh; testthat and withr come with zukomp's
# own Suggests. A file, not `Rscript -e`, so it runs the same on Windows.
# pak comes with r-lib/actions/setup-r-dependencies.
pak::pak(c("github::pedrobtz/zuxml", "github::pedrobtz/zucrypt"), ask = FALSE)
