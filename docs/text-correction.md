# YanFlow 保守纠错与评测

顺序是原始 ASR → 自定义词典 → 可选 MacBERT4CSC → 注入 / 气泡。
没有自由改写、补词、删除或自动全局音近词猜测。较低的实际错误率需要真人语音和人工真值
验证，置信度门槛本身不能保证正确；默认只启用词典。

## 第一阶段：自定义词典

右键悬浮按钮 → 编辑自定义词典。首次编辑会把包内 `dictionary.tsv` 复制到
`%APPDATA%\YanFlow\dictionary.tsv`；保存后下一次识别重新读取。个人文件优先于包内模板，
升级包不会覆盖个人词典。不存在个人文件时使用包内模板。UTF-8 编码，列之间使用 TAB：

```text
case	openai	OpenAI
term	麦克伯特	MacBERT	中文纠错
phonetic	热此	热词	语音识别
```

- `case` 只改变 ASCII 大小写，英文单词边界匹配，不改 `my_openai`、`openai2`。
- `term` 是用户确认的精确别名，第四列可限制同句上下文。
- `phonetic` 是用户确认的中文音近误识别，至少两个汉字，第四列上下文必填。
  上下文必须出现在同一句中，且距匹配位置不超过 80 个 UTF-16 字符。
- 默认只附带少量大小写规范；模板中的中文规则是注释示例，不会生效。
- 最长匹配优先，单次扫描不连锁替换；冲突目标的同源规则全部停用。
- 规范术语自身及替换结果受保护。反引号/引号内容、路径、URL、邮件、标识符，以及含
  `=`、`;`、`{}` 的代码行受保护。普通自然语言与裸代码标识符并不总能从文字判别；
  对需要完全保持的代码使用反引号。保护策略宁可少改。
- 文件损坏或非 UTF-8 时忽略词典，不猜测修复；文件上限 1 MiB，规则上限 2048 条。

建议从实际错误加入带上下文的映射。不要把“的→地”一类高歧义通用规则加入词典。

### 纠正并记住与应用词库

右键“纠正并记住”：确认原始误词、正确词及可选上下文，然后“记住并复制”。默认只用于
产生这次结果的目标应用；也可明确选择“通用（所有应用）”。中文单字替换必须填写上下文。
新确认的同源规则替换该词库中的旧映射。代码保护继续生效。

保存后复制本次正确文本，气泡中的对应结果也会更新；已输入到其他窗口的内容由用户粘贴
更正。打开对话框会停止录音，正在处理的新结果会保留。自动 ASR/纠错结果不被手工结果
覆盖：导出对照中的 `final` 仍是自动结果，手工内容另存为 `manual_correction`。

右键“应用词库”可以为当前应用选择“自动”“通用”“通用＋开发”。自动模式为 VS Code、
Visual Studio、部分 JetBrains IDE 和终端选择开发词库，其他应用用通用词库。只读取目标
进程的可执行文件名，不读取文档、项目文件或编辑器内容；同名程序共享应用词库。

叠加顺序：通用 → 开发（选择时）→ 应用专用。后一层的明确映射覆盖前一层的同源规则；
每一层内部的冲突规则仍停用。规则在下一次处理时重新读取，文件使用 UTF-8：
合并后的规则上限为 2048；达到上限时优先保留应用专用规则，其次开发规则，最后通用规则。

- 通用：`%APPDATA%\YanFlow\dictionary.tsv`，缺失时使用包内模板。
- 开发：`%APPDATA%\YanFlow\dictionaries\development.tsv`，缺失时使用包内 `dictionary-development.tsv`。
- 应用专用：`%APPDATA%\YanFlow\dictionaries\apps\<应用.exe>.tsv`，从菜单编辑或确认纠正时创建。
- 应用选择保存在 `settings.ini` 的 `[DictionaryApplications]` 中。

保存会校验规则、检查现有文件、持有短暂的文件锁，并以临时文件原子替换；文件损坏、
目录不可写或其他进程正在保存时保留原词典并提示。录音开始时记录目标应用，识别结果
若已切换到其他应用，则进入气泡，避免把应用专用纠正写进另一个应用。

## 音频边界与按住短词

每次推理保持 8 秒上限。长录音在约 6–7.6 秒范围优先寻找至少 80 ms 的低能量停顿并在
停顿中间切分；没有停顿时，两侧各保留 200 ms 上下文。免按住模式同样保留边界上下文，
按住模式仍在松开后才识别。连续切分的额外音频处理约有数个百分点开销，需要按机器测量。

worker 协议 v2 可返回 CTC token 的样本位置（时间分辨率约 60 ms）。边界合并只对共享
音频附近的相同 token 做有序一对一对齐，不对全文做字符串去重；重复 token 数量有歧义
时保留。CTC 位置可能漂移，对齐也可能漏合并或误匹配，仍需真实语音回归验证。

按住模式允许至少 80 ms 录音、至少 3 个有声采集帧的短词进入复核。低于约 350 ms 的
有声内容，且去掉过长尾部静音后整段不超过 1 秒时，使用单独的 FSMN-VAD 判定：至少
80 ms 连续帧的人声概率 ≥ 0.90；通常路径仍为原有约 150 ms、概率 ≥ 0.75 的规则。
复核通过后保留整段短录音，包括清辅音；必要的静音填充离线完成，不等待麦克风。

短词不再直接因 350 ms 门槛丢弃，但语言判断与孤立音节仍可能出错。专项 smoke 验证了
300 ms 的“我”片段，同时把截取位置可能不完整的“房”片段误识别保留在报告里；不把
这个测试当作短词总体错误率已下降的证据。

## 第二阶段：可选 MacBERT4CSC

开发环境运行：

```powershell
build\windows\setup-yanflow-csc.cmd
build\windows\build-yanflow.cmd
```

然后右键启用“MacBERT 中文纠错”。此开关保存到
`%APPDATA%\YanFlow\settings.ini` 的 `[Correction] MacBERTEnabled`，默认 `0`。
原生独立 worker 用 CPU ONNX Runtime 1.20.1 和固定版本 MacBERT4CSC INT8 模型；构建资源
固定 revision 并验证 SHA-256。所有 DLL、模型、字表和许可证随可选包附带，终端用户无需
安装 Python、PyTorch 或 VC 运行时。约增加 100 MB 模型及运行时文件。

worker 常驻，仅在推理线程访问；开启后第一次识别懒加载，关闭后下一次处理释放。
初始化最多等待 30 秒，单次纠错最多等待 10 秒；失败后该会话回退到词典输出，菜单显示
不可用。关闭再启用会在下一次识别重试。超过 4096 个 UTF-16 字符的文本跳过第二阶段。
长文本按 256 个 token 分块，边界上下文较少，可能漏改。

每个候选必须同时满足：

- 原字符与候选都是基本区汉字，长度、位置不变，不允许增删文字。
- 候选 softmax ≥ 0.995，原字符概率 ≤ 0.001，领先第二名 ≥ 0.20。
- 原位置不是词典术语、英文、数字、标点或可识别的代码。
- 每条最多改 2 字，且最多约占可改汉字的 10%（短句最多 1 字）。超过预算整批不改。
- 非有限概率、错误位置、重复位置或协议异常不会进入最终文本。

这些概率未针对你的语音语料校准。门槛可能让模型几乎不改字，这是当前保守默认策略。

## 用真实对照决定是否开启

右键“保存最近识别对照”把最近一次 `raw` 与 `final` 存到桌面 JSONL，不自动存录音或历史。
把 `expected` 填为人工听写的正确文本。将多条合并成 UTF-8 JSONL，至少包含已识别正确的
句子、中英混合、代码、术语错误和普通中文错字，并预留未参与调词典的测试集。

开发机上可使用 Python 标准库评测工具（应用本身不需要 Python）：

```powershell
py scripts\evaluate-correction.py pairs.jsonl --report build\artifacts\correction-real.json
# 使用当前包内词典重新处理原始文本：
py scripts\evaluate-correction.py pairs.jsonl --exe build\artifacts\yanflow-windows-x64\yanflow.exe --report build\artifacts\correction-dictionary.json
py scripts\evaluate-correction.py pairs.jsonl --exe build\artifacts\yanflow-windows-x64\yanflow.exe --macbert --report build\artifacts\correction-macbert.json
```

保存的实际对照使用个人词典；`--exe` 重放固定使用包内词典，测试个人规则时复制该词典到
一个独立测试包目录。CLI `--correct-text input.txt output.txt [--macbert]` 不采集音频。
MacBERT 不可用时仍写入词典结果并返回 `32`，评测会报错，避免把回退当作模型证据。

报告包含原始/最终 CER、回归句数、正确句被改数，以及对齐估计的已正确字符受损数。
大小写、空格、标点都计入 CER；对齐有歧义，仍需人工查看逐条结果。只有最终 CER 更低且
正确字符受损数为零才通过 gate（退出码 `0`）；否则退出码 `1`。`tests/correction-synthetic.jsonl`
只是保护规则的合成示例，不代表真实 ASR 效果。

## 第三阶段：术语仍错得多再实验

当前 SenseVoice GGUF worker 使用贪心 CTC 解码，不接受 FunASR Python 的
`postprocess_hotwords` / `postprocess_hotword_file` 参数。
[FunASR 的该模块](https://github.com/modelscope/FunASR/blob/main/funasr/utils/postprocess_hotwords.py)
是在 ASR 后修改文字的后处理，明确区别于模型级热词；模糊音近匹配还依赖 pypinyin 和
rapidfuzz。它不是当前 worker 的现成热词解码器。

先统计真实测试集中剩余错误是否主要是术语，再对同一批音频/人工真值独立比较：
基线 ASR、第一阶段、第一+第二阶段、第三方热词方案。记录 CER、术语命中、正确内容误改、
延迟和内存。精确热词映射可通过第一阶段测试；模糊匹配和真正的声学/CTC 热词解码需要
另外固定第三方实现/资源并评估。尚未执行第三方热词对照，也未把 Python 加入正式包。

模型来源：[MacBERT4CSC](https://huggingface.co/shibing624/macbert4csc-base-chinese)、
[固定 ONNX 转换](https://huggingface.co/Xenova/macbert4csc-base-chinese/tree/7ebfe81cf502576e93c844b95354840b2ecb5c28)。
