#include "NxMatrix/Net/HttpClient.hpp"
#include "NxMatrix/Ui/App.hpp"

#include <curl/curl.h>
#include <pu/Plutonium>

extern "C" void userAppInit() {
    socketInitializeDefault();
    nxlinkStdio();
}

extern "C" void userAppExit() {
    socketExit();
}

int main() {
    // SDL2 hardware rendering, system shared fonts (no bundled fonts), single
    // player input (handheld + first controller).
    auto Options = pu::ui::render::RendererInitOptions(SDL_INIT_EVERYTHING, pu::ui::render::RendererHardwareFlags);
    Options.UseImage(pu::ui::render::ImgAllFlags);
    Options.UseRomfs(); // holds the CA bundle (romfs:/cacert.pem)
    Options.SetPlServiceType(PlServiceType_User);
    Options.AddDefaultAllSharedFonts();
    Options.AddExtraDefaultFontSize(22);
    Options.AddExtraDefaultFontSize(24);
    Options.SetInputPlayerCount(1);
    Options.AddInputNpadStyleTag(HidNpadStyleSet_NpadStandard);
    Options.AddInputNpadIdType(HidNpadIdType_Handheld);
    Options.AddInputNpadIdType(HidNpadIdType_No1);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    NxMatrix::Net::HttpClient::Instance().SetCaBundlePath("romfs:/cacert.pem");
    {
        auto Renderer = pu::ui::render::Renderer::New(Options);
        auto App = NxMatrix::Ui::NxMatrixApp::New(Renderer);

        // Load() must be called before Show(), or nothing is rendered.
        const auto Rc = App->Load();
        if(R_FAILED(Rc)) {
            diagAbortWithResult(Rc);
        }
        App->Show();
    }
    curl_global_cleanup();
    return 0;
}