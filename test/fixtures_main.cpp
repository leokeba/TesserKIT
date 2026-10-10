// Writes the schemas of TesserKIT's services, with their hints, as golden
// fixtures (docs/DESIGN.md section 12): ./tesserkit_fixtures <dir>
#include <stdio.h>
#include <sys/stat.h>

#include "rig.h"

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "fixtures";
    mkdir(dir, 0755);
    rig::Rig r;
    tesserkit::System system(r.dev);
    tesserkit::Network network(r.dev);
    tesserkit::Time time(r.dev);
    tesserkit::Firmware firmware(r.dev);
    tesserkit::Backup backup(r.dev);
    if (!r.dev.begin()) return 1;
    rig::Reply s = r.schema("/");
    if (s.status != rig::Status::Ok) return 1;
    std::string path = std::string(dir) + "/sys.json";
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return 1;
    fputs(s.body.c_str(), f);
    fputc('\n', f);
    fclose(f);
    printf("wrote %s (%zu bytes)\n", path.c_str(), s.body.size());
    return 0;
}
