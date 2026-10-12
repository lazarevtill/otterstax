from conan import ConanFile
from conan.tools.gnu import Autotools, AutotoolsToolchain, AutotoolsDeps
from conan.tools.files import get, copy
from conan.errors import ConanInvalidConfiguration
import os


class UnixODBC(ConanFile):
    name = "odbc"
    version = "2.3.14"
    package_type = "shared-library"
    license = "LGPL-2.1-or-later"
    homepage = "https://www.unixodbc.org/"
    settings = "os", "arch", "compiler", "build_type"

    def validate(self):
        if self.settings.os == "Windows":
            raise ConanInvalidConfiguration("Windows uses the system ODBC driver manager")

    def source(self):
        get(self, url="https://www.unixodbc.org/unixODBC-2.3.14.tar.gz",
            sha256="4e2814de3e01fc30b0b9f75e83bb5aba91ab0384ee951286504bb70205524771",
            strip_root=True)

    def generate(self):
        toolchain = AutotoolsToolchain(self)
        toolchain.configure_args.extend(["--enable-shared", "--disable-static",
                                         "--disable-gui", "--disable-drivers"])
        toolchain.generate()
        AutotoolsDeps(self).generate()

    def build(self):
        autotools = Autotools(self)
        autotools.configure()
        autotools.make()

    def package(self):
        Autotools(self).install()
        copy(self, "COPYING*", self.source_folder, os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        self.cpp_info.libs = ["odbc"]
        self.cpp_info.set_property("cmake_file_name", "ODBC")
        self.cpp_info.set_property("cmake_target_name", "ODBC::ODBC")
