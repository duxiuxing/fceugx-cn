# 如何去掉对多语种的支持


主要需要修改 5 个地方：

## 1. 关闭代码中的多语种支持宏定义

确认 `Makefile.wii` 文件中，第 31 行 SHAREDFLAGS 的赋值代码不包含 `MULTI_LANGUAGE_SUPPORT` 的宏定义，即删除下面这段代码：

``` c++
-DMULTI_LANGUAGE_SUPPORT
```

## 2. 在代码中定义要使用的语种

在代码中，使用什么语种由枚举常量 `LANG_DEFAULT` 的取值决定。中文版本使用的语种是简体中文，因此在项目 `source\fceugx.h` 文件的第 80 行，可以看到下面的代码：

``` c++
LANG_DEFAULT = LANG_SIMP_CHINESE
```

## 3. 确认默认字体文件与语种对应

- 默认字体文件，位于项目的 `source\fonts\font.ttf`。启动编译之后，编译器会根据这个路径找到 `font.ttf`，并把它作为内部资源，合并到最后编译生成的 `.dol` 文件中；

- 中文版本各个分支的默认语种都是简体中文，所以默认字体文件 `source\fonts\font.ttf` 使用 `fonts\zh.ttf` 进行了替换。


## 4. 不需要打包字体文件

在 `cn-only` 分支的 Build 脚本（`workflows\build.yml`）中，所有拷贝字体文件的操作都会被注释掉：

```
# cp fonts/en.ttf dist/FCEUltraGX/apps/fceugx/
# cp fonts/jp.ttf dist/FCEUltraGX/apps/fceugx/
# cp fonts/ko.ttf dist/FCEUltraGX/apps/fceugx/
# cp fonts/zh.ttf dist/FCEUltraGX/apps/fceugx/
```

## 5. 删除其他语种的翻译文件

- 各个语种的翻译文件，位于项目的 `source\lang` 文件夹。启动编译之后，编译器会把这个文件夹下的 .lang 文件，逐个转换成 .h 文件和 .o 文件；

- 在 `cn-only` 分支的 `source\lang` 文件夹中，仅包含 `zh.lang` 一个文件，其余翻译文件已被删除；

- 在 `cn-only` 分支的 `source\filelist.h` 文件中，仅会引用中文翻译的 .h 文件：
    ```
    #include "zh_lang.h"
    ```

- 在 `cn-only` 分支的 `source\utils\gettext.cpp` 文件中，仅会使用中文翻译的字符串表：
    ``` c++
    file = (char*)zh_lang; eof = file + zh_lang_size;
    ```


## 6. 举一反三，以此类推

参考以上四步的做法，如果想构建一个日本语版本给你的老师用，相应的操作就是：

- 删除 `Makefile.wii` 文件中的宏定义
  ``` c++
  -DMULTI_LANGUAGE_SUPPORT
  ```
- 在 `source\fceugx.h` 文件中定义默认语种
  ``` c++
  LANG_DEFAULT = LANG_JAPANESE
  ```
- 替换默认字体文件，也就是用 `fonts\jp.ttf` 替换 `source\fonts\font.ttf`
- 移除 Build 脚本（`workflows\build.yml`）中拷贝字体文件的操作
  ```
  # cp fonts/en.ttf dist/FCEUltraGX/apps/fceugx/
  # cp fonts/jp.ttf dist/FCEUltraGX/apps/fceugx/
  # cp fonts/ko.ttf dist/FCEUltraGX/apps/fceugx/
  # cp fonts/zh.ttf dist/FCEUltraGX/apps/fceugx/
  ```
- 删除 `source\lang` 文件夹中除 `jp.lang` 以外的翻译文件；
- 在 `source\filelist.h` 文件中，仅会引用日文翻译的 .h 文件：
    ```
    #include "zh_lang.h"
    ```
- 在 `source\utils\gettext.cpp` 文件中，仅会使用日文翻译的字符串表：
    ``` c++
    file = (char*)jp_lang; eof = file + jp_lang_size;
    ```
