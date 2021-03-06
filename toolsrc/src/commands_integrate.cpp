#include "pch.h"

#include "vcpkg_Checks.h"
#include "vcpkg_Commands.h"
#include "vcpkg_Files.h"
#include "vcpkg_System.h"
#include "vcpkg_Util.h"

namespace vcpkg::Commands::Integrate
{
    static const std::array<fs::path, 2> old_system_target_files = {
        System::get_ProgramFiles_32_bit() / "MSBuild/14.0/Microsoft.Common.Targets/ImportBefore/vcpkg.nuget.targets",
        System::get_ProgramFiles_32_bit() / "MSBuild/14.0/Microsoft.Common.Targets/ImportBefore/vcpkg.system.targets"};
    static const fs::path system_wide_targets_file =
        System::get_ProgramFiles_32_bit() / "MSBuild/Microsoft.Cpp/v4.0/V140/ImportBefore/Default/vcpkg.system.props";

    static std::string create_appdata_targets_shortcut(const std::string& target_path) noexcept
    {
        return Strings::format(R"###(
<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <Import Condition="Exists('%s') and '$(VCPkgLocalAppDataDisabled)' == ''" Project="%s" />
</Project>
)###",
                               target_path,
                               target_path);
    }

    static std::string create_system_targets_shortcut() noexcept
    {
        return R"###(
<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <!-- version 1 -->
  <PropertyGroup>
    <VCLibPackagePath Condition="'$(VCLibPackagePath)' == ''">$(LOCALAPPDATA)\vcpkg\vcpkg.user</VCLibPackagePath>
  </PropertyGroup>
  <Import Condition="'$(VCLibPackagePath)' != '' and Exists('$(VCLibPackagePath).targets')" Project="$(VCLibPackagePath).targets" />
</Project>
)###";
    }

    static std::string create_nuget_targets_file_contents(const fs::path& msbuild_vcpkg_targets_file) noexcept
    {
        const std::string as_string = msbuild_vcpkg_targets_file.string();

        return Strings::format(R"###(
<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <Import Project="%s" Condition="Exists('%s')" />
  <Target Name="CheckValidPlatform" BeforeTargets="Build">
    <Error Text="Unsupported architecture combination. Remove the 'vcpkg' nuget package." Condition="'$(VCPkgEnabled)' != 'true' and '$(VCPkgDisableError)' == ''"/>
  </Target>
</Project>
)###",
                               as_string,
                               as_string);
    }

    static std::string create_nuget_props_file_contents() noexcept
    {
        return R"###(
<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <VCPkgLocalAppDataDisabled>true</VCPkgLocalAppDataDisabled>
  </PropertyGroup>
</Project>
)###";
    }

    static std::string get_nuget_id(const fs::path& vcpkg_root_dir)
    {
        std::string dir_id = vcpkg_root_dir.generic_string();
        std::replace(dir_id.begin(), dir_id.end(), '/', '.');
        dir_id.erase(1, 1); // Erasing the ":"

        // NuGet id cannot have invalid characters. We will only use alphanumeric and dot.
        Util::erase_remove_if(dir_id, [](char c) { return !isalnum(c) && (c != '.'); });

        const std::string nuget_id = "vcpkg." + dir_id;
        return nuget_id;
    }

    static std::string create_nuspec_file_contents(const fs::path& vcpkg_root_dir,
                                                   const std::string& nuget_id,
                                                   const std::string& nupkg_version)
    {
        static constexpr auto content_template = R"(
<package>
    <metadata>
        <id>@NUGET_ID@</id>
        <version>@VERSION@</version>
        <authors>vcpkg</authors>
        <description>
            This package imports all libraries currently installed in @VCPKG_DIR@. This package does not contain any libraries and instead refers to the folder directly (like a symlink).
        </description>
    </metadata>
    <files>
        <file src="vcpkg.nuget.props" target="build\native\@NUGET_ID@.props" />
        <file src="vcpkg.nuget.targets" target="build\native\@NUGET_ID@.targets" />
    </files>
</package>
)";

        std::string content = std::regex_replace(content_template, std::regex("@NUGET_ID@"), nuget_id);
        content = std::regex_replace(content, std::regex("@VCPKG_DIR@"), vcpkg_root_dir.string());
        content = std::regex_replace(content, std::regex("@VERSION@"), nupkg_version);
        return content;
    }

    enum class ElevationPromptChoice
    {
        YES,
        NO
    };

    static ElevationPromptChoice elevated_cmd_execute(const std::string& param)
    {
        SHELLEXECUTEINFO shExInfo = {0};
        shExInfo.cbSize = sizeof(shExInfo);
        shExInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
        shExInfo.hwnd = nullptr;
        shExInfo.lpVerb = "runas";
        shExInfo.lpFile = "cmd"; // Application to start

        shExInfo.lpParameters = param.c_str(); // Additional parameters
        shExInfo.lpDirectory = nullptr;
        shExInfo.nShow = SW_HIDE;
        shExInfo.hInstApp = nullptr;

        if (!ShellExecuteExA(&shExInfo))
        {
            return ElevationPromptChoice::NO;
        }
        if (shExInfo.hProcess == nullptr)
        {
            return ElevationPromptChoice::NO;
        }
        WaitForSingleObject(shExInfo.hProcess, INFINITE);
        CloseHandle(shExInfo.hProcess);
        return ElevationPromptChoice::YES;
    }

    static fs::path get_appdata_targets_path()
    {
        static const fs::path local_app_data =
            fs::path(System::get_environment_variable(L"LOCALAPPDATA").value_or_exit(VCPKG_LINE_INFO));
        return local_app_data / "vcpkg" / "vcpkg.user.targets";
    }

    static void integrate_install(const VcpkgPaths& paths)
    {
        auto& fs = paths.get_filesystem();

        // TODO: This block of code should eventually be removed
        for (auto&& old_system_wide_targets_file : old_system_target_files)
        {
            if (fs.exists(old_system_wide_targets_file))
            {
                const std::string param =
                    Strings::format(R"(/c DEL "%s" /Q > nul)", old_system_wide_targets_file.string());
                ElevationPromptChoice user_choice = elevated_cmd_execute(param);
                switch (user_choice)
                {
                    case ElevationPromptChoice::YES: break;
                    case ElevationPromptChoice::NO:
                        System::println(System::Color::warning, "Warning: Previous integration file was not removed");
                        Checks::exit_fail(VCPKG_LINE_INFO);
                    default: Checks::unreachable(VCPKG_LINE_INFO);
                }
            }
        }

        std::error_code ec;
        const fs::path tmp_dir = paths.buildsystems / "tmp";
        fs.create_directory(paths.buildsystems, ec);
        fs.create_directory(tmp_dir, ec);

        bool should_install_system = true;
        const Expected<std::string> system_wide_file_contents = fs.read_contents(system_wide_targets_file);
        if (auto contents_data = system_wide_file_contents.get())
        {
            std::regex re(R"###(<!-- version (\d+) -->)###");
            std::match_results<std::string::const_iterator> match;
            auto found = std::regex_search(*contents_data, match, re);
            if (found)
            {
                int ver = atoi(match[1].str().c_str());
                if (ver >= 1) should_install_system = false;
            }
        }

        if (should_install_system)
        {
            const fs::path sys_src_path = tmp_dir / "vcpkg.system.targets";
            fs.write_contents(sys_src_path, create_system_targets_shortcut());

            const std::string param = Strings::format(R"(/c mkdir "%s" & copy "%s" "%s" /Y > nul)",
                                                      system_wide_targets_file.parent_path().string(),
                                                      sys_src_path.string(),
                                                      system_wide_targets_file.string());
            ElevationPromptChoice user_choice = elevated_cmd_execute(param);
            switch (user_choice)
            {
                case ElevationPromptChoice::YES: break;
                case ElevationPromptChoice::NO:
                    System::println(System::Color::warning, "Warning: integration was not applied");
                    Checks::exit_fail(VCPKG_LINE_INFO);
                default: Checks::unreachable(VCPKG_LINE_INFO);
            }

            Checks::check_exit(VCPKG_LINE_INFO,
                               fs.exists(system_wide_targets_file),
                               "Error: failed to copy targets file to %s",
                               system_wide_targets_file.string());
        }

        const fs::path appdata_src_path = tmp_dir / "vcpkg.user.targets";
        fs.write_contents(appdata_src_path,
                          create_appdata_targets_shortcut(paths.buildsystems_msbuild_targets.string()));
        auto appdata_dst_path = get_appdata_targets_path();

        auto rc = fs.copy_file(appdata_src_path, appdata_dst_path, fs::copy_options::overwrite_existing, ec);

        if (!rc || ec)
        {
            System::println(System::Color::error,
                            "Error: Failed to copy file: %s -> %s",
                            appdata_src_path.string(),
                            appdata_dst_path.string());
            Checks::exit_fail(VCPKG_LINE_INFO);
        }
        System::println(System::Color::success, "Applied user-wide integration for this vcpkg root.");
        const fs::path cmake_toolchain = paths.buildsystems / "vcpkg.cmake";
        System::println("\n"
                        "All MSBuild C++ projects can now #include any installed libraries.\n"
                        "Linking will be handled automatically.\n"
                        "Installing new libraries will make them instantly available.\n"
                        "\n"
                        "CMake projects should use -DCMAKE_TOOLCHAIN_FILE=%s",
                        cmake_toolchain.generic_string());

        Checks::exit_success(VCPKG_LINE_INFO);
    }

    static void integrate_remove(Files::Filesystem& fs)
    {
        const fs::path path = get_appdata_targets_path();

        std::error_code ec;
        bool was_deleted = fs.remove(path, ec);

        Checks::check_exit(VCPKG_LINE_INFO, !ec, "Error: Unable to remove user-wide integration: %d", ec.message());

        if (was_deleted)
        {
            System::println(System::Color::success, "User-wide integration was removed");
        }
        else
        {
            System::println(System::Color::success, "User-wide integration is not installed");
        }

        Checks::exit_success(VCPKG_LINE_INFO);
    }

    static void integrate_project(const VcpkgPaths& paths)
    {
        auto& fs = paths.get_filesystem();

        const fs::path& nuget_exe = paths.get_nuget_exe();

        const fs::path& buildsystems_dir = paths.buildsystems;
        const fs::path tmp_dir = buildsystems_dir / "tmp";
        std::error_code ec;
        fs.create_directory(buildsystems_dir, ec);
        fs.create_directory(tmp_dir, ec);

        const fs::path targets_file_path = tmp_dir / "vcpkg.nuget.targets";
        const fs::path props_file_path = tmp_dir / "vcpkg.nuget.props";
        const fs::path nuspec_file_path = tmp_dir / "vcpkg.nuget.nuspec";
        const std::string nuget_id = get_nuget_id(paths.root);
        const std::string nupkg_version = "1.0.0";

        fs.write_contents(targets_file_path, create_nuget_targets_file_contents(paths.buildsystems_msbuild_targets));
        fs.write_contents(props_file_path, create_nuget_props_file_contents());
        fs.write_contents(nuspec_file_path, create_nuspec_file_contents(paths.root, nuget_id, nupkg_version));

        // Using all forward slashes for the command line
        const std::wstring cmd_line = Strings::wformat(LR"("%s" pack -OutputDirectory "%s" "%s" > nul)",
                                                       nuget_exe.native(),
                                                       buildsystems_dir.native(),
                                                       nuspec_file_path.native());

        const int exit_code = System::cmd_execute_clean(cmd_line);

        const fs::path nuget_package = buildsystems_dir / Strings::format("%s.%s.nupkg", nuget_id, nupkg_version);
        Checks::check_exit(
            VCPKG_LINE_INFO, exit_code == 0 && fs.exists(nuget_package), "Error: NuGet package creation failed");
        System::println(System::Color::success, "Created nupkg: %s", nuget_package.string());

        auto source_path = buildsystems_dir.u8string();
        source_path = std::regex_replace(source_path, std::regex("`"), "``");

        System::println(R"(
With a project open, go to Tools->NuGet Package Manager->Package Manager Console and paste:
    Install-Package %s -Source "%s"
)",
                        nuget_id,
                        source_path);

        Checks::exit_success(VCPKG_LINE_INFO);
    }

    const char* const INTEGRATE_COMMAND_HELPSTRING =
        "  vcpkg integrate install         Make installed packages available user-wide. Requires admin privileges on "
        "first use\n"
        "  vcpkg integrate remove          Remove user-wide integration\n"
        "  vcpkg integrate project         Generate a referencing nuget package for individual VS project use\n";

    void perform_and_exit(const VcpkgCmdArguments& args, const VcpkgPaths& paths)
    {
        static const std::string example = Strings::format("Commands:\n"
                                                           "%s",
                                                           INTEGRATE_COMMAND_HELPSTRING);
        args.check_exact_arg_count(1, example);
        args.check_and_get_optional_command_arguments({});

        if (args.command_arguments[0] == "install")
        {
            return integrate_install(paths);
        }
        if (args.command_arguments[0] == "remove")
        {
            return integrate_remove(paths.get_filesystem());
        }
        if (args.command_arguments[0] == "project")
        {
            return integrate_project(paths);
        }

        Checks::exit_with_message(VCPKG_LINE_INFO, "Unknown parameter %s for integrate", args.command_arguments[0]);
    }
}
