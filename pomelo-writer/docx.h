/* SPDX-License-Identifier: GPL-3.0-or-later */
/* docx.h — 讀寫 Word 的 .docx（OOXML WordprocessingML）。
 * 已知限制：不處理具名樣式的繼承（只看每個段落／文字跑上直接標出來的格式）、
 * 不支援頁首、追蹤修訂；表格儲存格裡原本有好幾個段落時，文字會接在一起變成一段；
 * 圖片只認得 PNG／JPEG，而且只支援整段只有一張圖（跟文字同一段混排的圖片讀的時候會跳過）。
 */
#ifndef POMELO_WRITER_DOCX_H
#define POMELO_WRITER_DOCX_H
#include "doc.h"

Document *docx_read(const char *path);
int docx_write(const Document *d, const char *path);   /* 成功回傳 1 */

#endif
