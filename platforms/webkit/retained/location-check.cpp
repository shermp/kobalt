#include "location-checks.h"
#include "location-script.h"
#include "page-model.h"

#include <jsc/jsc.h>

#include <cstdio>
#include <string>

int main()
{
    g_autoptr(JSCContext) context = jsc_context_new();
    const auto script = std::string(sourceLocationScript) + locationChecks;
    g_autoptr(JSCValue) result = jsc_context_evaluate(context, script.c_str(), script.size());
    if (auto *exception = jsc_context_get_exception(context)) {
        std::fprintf(stderr, "Source location check failed: %s\n", jsc_exception_get_message(exception));
        return 1;
    }
    if (!result || !jsc_value_is_boolean(result) || !jsc_value_to_boolean(result))
        return 1;

    using retained::PageModel;
    using Precision = PageModel::Precision;
    PageModel model({{"OPS/chapter1.xhtml", 4096}, {"OPS/chapter2.xhtml", 8192}});
    if (model.total().precision != Precision::Unknown || model.chapter("other.xhtml")
        || model.chapter("OPS/chapter2.xhtml") != 1 || !model.observe(0, 7)
        || model.total().precision != Precision::Unknown)
        return 1;
    const auto run = [] { return false; };
    const auto cancel = [] { return true; };
    if (!model.indexNext(run) || model.indexNext(cancel)
        || model.count(1).precision != Precision::Unknown || !model.indexNext(run)
        || model.total().precision != Precision::Estimated || model.total().pages != 21
        || !model.observe(1, 11) || model.total().precision != Precision::Exact
        || model.total().pages != 18)
        return 1;
    model.resetLayout();
    if (model.total().precision != Precision::Unknown || model.indexNext(run)
        || model.count(0).precision != Precision::Unknown)
        return 1;
    PageModel other({{"same/section.xhtml", 0}});
    if (other.total().precision != Precision::Unknown || other.chapter("OPS/chapter1.xhtml"))
        return 1;
    std::puts("Source location Unicode and chapter page model checks passed");
}
