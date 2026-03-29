#include <drogon/drogon.h>
#include "controllers/AuthController.h"
#include "controllers/FilesystemController.h"
#include "controllers/UploadController.h"

int main() {
    drogon::app()
        .loadConfigFile("home_path/nas/nas_main/config.json")
        .run();
}
