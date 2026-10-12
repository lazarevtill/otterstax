from conan import ConanFile
from conan.tools.files import get, copy
from conan.errors import ConanInvalidConfiguration
import os


class SnowflakeODBC(ConanFile):
    name = "snowflake-odbc"
    version = "4.0.0"
    package_type = "shared-library"
    license = "Apache-2.0"
    homepage = "https://github.com/snowflakedb/drivers"
    settings = "os", "arch"
    exports_sources = "LICENSE"
    _archives = {
        "x86_64": ("x86_64", "e615d85b80d38ae1a14aa87991faafdca4b01c892cd2a223256e1fa65128d694"),
        "armv8": ("aarch64", "c483f96887adb8e03b2945bef8e9f91a705bc5cceff445f8d9152c57cdd2065a"),
    }

    def validate(self):
        if self.settings.os != "Linux" or str(self.settings.arch) not in self._archives:
            raise ConanInvalidConfiguration("Bundled Snowflake ODBC requires Linux x86_64 or armv8")

    def build(self):
        arch, checksum = self._archives[str(self.settings.arch)]
        get(self, url=f"https://github.com/snowflakedb/drivers/releases/download/snowflake-odbc/{self.version}/snowflake-odbc-{self.version}.{arch}.tar.gz",
            sha256=checksum)

    def package(self):
        root = os.path.join(self.build_folder, "usr", "lib64", "snowflake", "odbc")
        copy(self, "*.so", os.path.join(root, "lib"), os.path.join(self.package_folder, "lib"))
        copy(self, "*.h", os.path.join(root, "include"), os.path.join(self.package_folder, "include"))
        copy(self, "LICENSE", self.source_folder, os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        # Loaded by ODBC at runtime; never link its ODBC entry points over the
        # driver manager's symbols in the application.
        self.cpp_info.libs = []
