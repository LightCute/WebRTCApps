import os

# ====================== 【全局配置区】 集中管理所有规则 ======================
# 输出文件名
OUTPUT_FILE = "all_code.txt"
# 跳过的目录名（文件夹）
SKIP_DIRS = {"build", ".git", ".vscode", "cmake-build-debug", "third_party"}
# ✅ 新增：需要收集的文件后缀（全局统一管理，增删改这里即可）
COLLECT_EXTENSIONS = (".h", ".cpp", ".hpp", ".c", ".cc")
# ✅ 新增：需要屏蔽的文件（完整文件名，带后缀，区分大小写）
SKIP_FILES = {"test.cpp", "demo.h", "temp.c"}  # 按需修改
# ==========================================================================

# 存储目录结构：key为目录路径，value为该目录下的有效文件列表
dir_structure = {}
total_lines = 0
file_count = 0

# 第一步：遍历目录，收集目录结构和文件信息
for root, dirs, files in os.walk("."):
    # 移除要跳过的目录（不遍历这些目录）
    dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
    
    # 筛选文件：后缀匹配 + 不在屏蔽文件列表中
    valid_files = [
        f for f in files 
        if f.endswith(COLLECT_EXTENSIONS) and f not in SKIP_FILES
    ]
    if valid_files:  # 只记录有有效文件的目录
        dir_structure[root] = sorted(valid_files)

# 第二步：写入内容到输出文件
with open(OUTPUT_FILE, "w", encoding="utf-8") as out:
    # 1. 写入目录结构概览
    out.write("========== 工程目录结构（仅含代码文件）==========\n\n")
    for dir_path, files in dir_structure.items():
        out.write(f"📂 {dir_path}:\n")
        for file_name in files:
            out.write(f"  └─ {file_name}\n")
        out.write("\n")
    
    # 分隔线
    out.write("=" * 50 + "\n")
    out.write("========== 所有代码文件内容 ==========\n\n")

    # 2. 写入每个文件的内容
    for root, dirs, files in os.walk("."):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        
        for name in files:
            # 统一筛选规则：后缀匹配 + 不屏蔽
            if name.endswith(COLLECT_EXTENSIONS) and name not in SKIP_FILES:
                path = os.path.join(root, name)

                try:
                    with open(path, "r", encoding="utf-8", errors="ignore") as f:
                        lines = f.readlines()
                except Exception as e:
                    print(f"Skip {path}: {e}")
                    continue

                line_count = len(lines)
                total_lines += line_count
                file_count += 1

                out.write(f"\n========== {path} ==========\n")
                out.write(f"Lines: {line_count}\n\n")
                out.writelines(lines)

    # 3. 结尾统计
    out.write("\n\n=====================================\n")
    out.write(f"Total files: {file_count}\n")
    out.write(f"Total lines: {total_lines}\n")

# 控制台输出
print("=====================================")
print(f"已收集文件数: {file_count}")
print(f"总行数: {total_lines}")
print(f"结果已保存到: {OUTPUT_FILE}")
