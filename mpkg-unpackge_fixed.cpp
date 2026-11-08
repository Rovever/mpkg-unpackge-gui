#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <stdexcept>
#include <bitset>
#include <sstream>

namespace fs = std::filesystem;

// 定义缓冲区大小为1MB，您可以根据需要调整
const size_t BUFFER_SIZE = 1024 * 1024;

// 读取4字节的整数
uint32_t readInt32(std::ifstream& inputStream) {
    char buffer[4];
    inputStream.read(buffer, 4);
    if (inputStream.gcount() != 4) {
        throw std::runtime_error("读取整数字节不足");
    }
    uint32_t result = 0;
    result |= (static_cast<unsigned char>(buffer[0]) & 0xFF);
    result |= (static_cast<unsigned char>(buffer[1]) & 0xFF) << 8;
    result |= (static_cast<unsigned char>(buffer[2]) & 0xFF) << 16;
    result |= (static_cast<unsigned char>(buffer[3]) & 0xFF) << 24;
    return result;
}

// 复制流中的数据到目标流
void copyStreamData(std::ifstream& input, std::ofstream& output, size_t length) {
    std::vector<char> buffer(BUFFER_SIZE);
    size_t remaining = length;

    // 逐块读取并写入
    while (remaining > 0) {
        size_t toRead = std::min(BUFFER_SIZE, remaining);
        input.read(buffer.data(), toRead);
        size_t actualRead = input.gcount();
        if (actualRead == 0) {
            throw std::runtime_error("文件提前终止，预期长度：" + std::to_string(length) + " 剩余：" + std::to_string(remaining));
        }
        output.write(buffer.data(), actualRead);
        remaining -= actualRead;
    }
}

// 解包单个MPKG文件
void unpackMpkg(const fs::path& inputFile, const fs::path& outputDir) {
    std::ifstream inputStream(inputFile, std::ios::binary);
    if (!inputStream.is_open()) {
        throw std::runtime_error("无法打开文件: " + inputFile.string());
    }

    // 创建输出文件夹，以MPKG文件名为文件夹名
    fs::path unpackedFolder = outputDir / inputFile.stem();
    if (!fs::exists(unpackedFolder)) {
        fs::create_directories(unpackedFolder);
    }

    // 读取头部信息
    uint32_t headerLength = readInt32(inputStream);
    std::vector<char> headerBytes(headerLength);
    inputStream.read(headerBytes.data(), headerLength);
    std::string headerStr(headerBytes.begin(), headerBytes.end());
    std::cout << "文件格式版本：" << headerStr << std::endl;

    // 读取文件数量
    uint32_t fileCount = readInt32(inputStream);
    std::cout << "发现文件数量：" << fileCount << std::endl;

    // 构建文件列表
    std::vector<std::pair<std::string, uint32_t>> fileList;
    for (uint32_t i = 0; i < fileCount; ++i) {
        uint32_t nameLength = readInt32(inputStream);
        std::vector<char> nameBytes(nameLength);
        inputStream.read(nameBytes.data(), nameLength);
        std::string fileName(nameBytes.begin(), nameBytes.end());

        // 跳过未知字段
        inputStream.ignore(4);

        // 读取文件大小
        uint32_t fileSize = readInt32(inputStream);
        fileList.push_back({fileName, fileSize});
    }

    // 逐个解包文件到指定文件夹
    for (size_t i = 0; i < fileList.size(); ++i) {
        const auto& [fileName, fileSize] = fileList[i];
        std::cout << "正在解包文件 " << i + 1 << "/" << fileList.size() << " : " << fileName << std::endl;

        // 创建文件夹，确保路径存在
        fs::path fullOutputPath = unpackedFolder / fileName;
        fs::path parentDir = fullOutputPath.parent_path();
        if (!fs::exists(parentDir)) {
            fs::create_directories(parentDir); // 确保所有文件夹路径存在
        }

        // 打开输出文件
        std::ofstream outputStream(fullOutputPath, std::ios::binary);
        if (!outputStream.is_open()) {
            throw std::runtime_error("无法创建输出文件: " + fullOutputPath.string());
        }

        // 复制数据
        copyStreamData(inputStream, outputStream, fileSize);
        std::cout << "文件解包完成: " << fileName << std::endl;
    }

    std::cout << "解包成功完成！" << std::endl;
}

int main() {
    try {
        // 提示用户输入文件夹路径
        std::cout << "请输入包含MPKG文件的文件夹路径：";
        std::string inputFolder;
        std::getline(std::cin, inputFolder);
        fs::path inputFolderPath(inputFolder);

        // 检查文件夹是否存在
        if (!fs::exists(inputFolderPath) || !fs::is_directory(inputFolderPath)) {
            std::cerr << "无效的文件夹路径！" << std::endl;
            return 1;
        }

        // 指定输出路径
        fs::path outputDir = inputFolder + "/unpacked";

        // 遍历文件夹中的所有MPKG文件并解包
        for (const auto& entry : fs::directory_iterator(inputFolderPath)) {
            if (entry.is_regular_file() && entry.path().extension() == ".mpkg") {
                std::cout << "正在处理文件: " << entry.path().filename().string() << std::endl;
                try {
                    unpackMpkg(entry.path(), outputDir);
                } catch (const std::exception& e) {
                    std::cerr << "解包失败: " << e.what() << std::endl;
                }
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "程序出错: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}