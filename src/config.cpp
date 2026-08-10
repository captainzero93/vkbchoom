#include "config.hpp"
#include "platform_win32.hpp"

#include <array>
#include <sstream>
#include <locale>
#include <vector>

namespace vkbChoom
{
    Config::Config()
    {
        // Custom config file path (e.g. set VKBCHOOM_CONFIG_FILE=C:\path\to\vkbchoom.conf)
        const char* tmpConfEnv       = std::getenv("VKBCHOOM_CONFIG_FILE");
        std::string customConfigFile = tmpConfEnv ? std::string(tmpConfEnv) : "";

        // The old version's comment claimed it looked "next to the layer DLL
        // as a fallback" -- it did not. The array had two entries and the
        // only non-override one was the bare relative string "vkbchoom.conf",
        // resolved against the process working directory. If the CWD was not
        // the game folder, the config was silently not found, "no good config
        // file" went to a log that was itself misplaced for the same reason,
        // and the layer ran on defaults (renderMode unset -> not "transfer").
        // Implemented properly now, with absolute paths.
        std::vector<std::string> configPath;

        if (!customConfigFile.empty())
            configPath.push_back(customConfigFile);

        if (std::string dir = moduleDir(); !dir.empty())
            configPath.push_back(dir + "vkbchoom.conf"); // next to smaa_layer.dll

        if (std::string dir = exeDir(); !dir.empty())
            configPath.push_back(dir + "vkbchoom.conf"); // next to the game exe

        configPath.push_back("vkbchoom.conf"); // legacy CWD-relative, last resort

        for (const auto& cFile : configPath)
        {
            std::ifstream configFile(cFile);
            if (!configFile.good())
            {
                Logger::debug("config file not found at: " + cFile);
                continue;
            }

            Logger::info("config file: " + cFile);
            readConfigFile(configFile);
            return;
        }

        Logger::err("no config file found in any of " + std::to_string(configPath.size())
                    + " locations -- running on built-in defaults");
    }

    Config::Config(const Config& other)
    {
        this->options = other.options;
    }

    void Config::readConfigFile(std::ifstream& stream)
    {
        std::string line;

        while (std::getline(stream, line))
        {
            readConfigLine(line);
        }
    }

    void Config::readConfigLine(std::string line)
    {
        std::string key;
        std::string value;

        bool inQuotes    = false;
        bool foundEquals = false;

        auto appendChar = [&key, &value, &foundEquals](const char& newChar) {
            if (foundEquals)
                value += newChar;
            else
                key += newChar;
        };

        for (const char& nextChar : line)
        {
            if (inQuotes)
            {
                if (nextChar == '"')
                    inQuotes = false;
                else
                    appendChar(nextChar);
                continue;
            }
            switch (nextChar)
            {
                case '#': goto BREAK;
                case '"': inQuotes = true; break;
                case '\t':
                case ' ': break;
                case '=': foundEquals = true; break;
                default: appendChar(nextChar); break;
            }
        }

    BREAK:

        if (!key.empty() && !value.empty())
        {
            Logger::info(key + " = " + value);
            options[key] = value;
        }
    }

    void Config::parseOption(const std::string& option, int32_t& result)
    {
        auto found = options.find(option);
        if (found != options.end())
        {
            try
            {
                result = std::stoi(found->second);
            }
            catch (...)
            {
                Logger::warn("invalid int32_t value for: " + option);
            }
        }
    }

    void Config::parseOption(const std::string& option, float& result)
    {
        auto found = options.find(option);
        if (found != options.end())
        {
            // TODO find a better float parsing way, std::stof has locale issues
            std::stringstream ss(found->second);
            ss.imbue(std::locale("C"));
            float value;
            ss >> value;

            bool failed = ss.fail();

            std::string rest;
            ss >> rest;
            if (failed || (!rest.empty() && rest != "f"))
            {
                Logger::warn("invalid float value for: " + option);
            }
            else
            {
                result = value;
            }
        }
    }

    void Config::parseOption(const std::string& option, bool& result)
    {
        auto found = options.find(option);
        if (found != options.end())
        {
            if (found->second == "True" || found->second == "true" || found->second == "1")
            {
                result = true;
            }
            else if (found->second == "False" || found->second == "false" || found->second == "0")
            {
                result = false;
            }
            else
            {
                Logger::warn("invalid bool value for: " + option);
            }
        }
    }

    void Config::parseOption(const std::string& option, std::string& result)
    {
        auto found = options.find(option);
        if (found != options.end())
        {
            result = found->second;
        }
    }

    void Config::parseOption(const std::string& option, std::vector<std::string>& result)
    {
        auto found = options.find(option);
        if (found != options.end())
        {
            result = {};
            std::stringstream stringStream(found->second);
            std::string       newString;
            while (getline(stringStream, newString, ':'))
            {
                result.push_back(newString);
            }
        }
    }
} // namespace vkbChoom
