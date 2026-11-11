// The native file dialogs (SDL3: the desktop portal, or zenity/kdialog, on Linux). SDL may call
// back on another thread, so the answer is copied and handed to the main thread with gp_post,
// gpuic's one thread-safe call, which runs it there (and drops it if the editor is gone by then).
#include "ui/dialog.h"

#include <SDL3/SDL.h>

#include <optional>
#include <string>

namespace rack::ui
{

namespace
{
    struct Request
    {
        gp_app   *app;
        gp_entity editor;
        bool      save;
    };
    struct Answer
    {
        bool        save = false, chosen = false;
        std::string path, error;
    };

    void on_main_thread(void *self, gp_ctx *cx, void *data)
    {
        auto *a = static_cast<Answer *>(data);
        static_cast<Editor *>(self)->file_chosen(gpuic::Ctx(cx), a->save,
            a->chosen ? std::optional<std::string>(a->path) : std::nullopt, a->error);
        delete a;
    }

    void SDLCALL on_any_thread(void *userdata, const char *const *files, int)
    {
        auto *r = static_cast<Request *>(userdata);
        auto *a = new Answer;
        a->save = r->save;
        if (!files)
            a->error = SDL_GetError(); // the dialog failed
        else if (files[0])
        {
            a->chosen = true;
            a->path = files[0];
        } // else: cancelled
        gp_post(r->app, r->editor, &on_main_thread, a);
        delete r;
    }

    const SDL_DialogFileFilter kFilters[]
        = { { "PulseRack patches", "rack" }, { "All files", "*" } };
} // namespace

void native_file_dialog(
    gpuic::Ctx cx, gpuic::Entity<Editor> editor, bool save, const std::string &suggested)
{
    auto       *r = new Request { cx.raw()->app, editor.id, save };
    const char *where = suggested.empty() ? nullptr : suggested.c_str();
    if (save)
        SDL_ShowSaveFileDialog(&on_any_thread, r, nullptr, kFilters, 2, where);
    else
        SDL_ShowOpenFileDialog(&on_any_thread, r, nullptr, kFilters, 2, where, false);
}

} // namespace rack::ui
