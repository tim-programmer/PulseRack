// PulseRack: the application. `pulserack [patch.rack]`
#include "ui/rack_ui.h"

#include <cstdio>
#include <cstring>

int main(int argc, char **argv)
{
    rack::ui::Options opt;
    for (int i = 1; i < argc; i++)
    {
        if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help"))
        {
            std::printf("usage: pulserack [patch.rack]\n");
            return 0;
        }
        opt.patch_path = argv[i];
    }
    int       status = 0;
    const int rc = gpuic::run(
        [&](gpuic::App app)
        {
            std::string error;
            if (!rack::ui::open_rack(app, opt, &error))
            {
                std::fprintf(stderr, "pulserack: %s\n", error.c_str());
                status = 1;
                app.quit();
            }
        });
    return status ? status : rc;
}
