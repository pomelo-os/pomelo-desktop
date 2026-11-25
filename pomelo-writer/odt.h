/* SPDX-License-Identifier: GPL-3.0-or-later */
/* odt.h — 讀寫 LibreOffice／OpenOffice Writer 的 .odt（OpenDocument Text）。
 * 已知限制同 docx.h：不處理具名樣式繼承、不支援頁首／追蹤修訂；
 * 表格儲存格裡原本有好幾個段落時，文字會接在一起變成一段；
 * 圖片只認得 PNG／JPEG，而且只支援整段只有一張圖（跟文字同一段混排的圖片讀的時候會跳過）。
 */
#ifndef POMELO_WRITER_ODT_H
#define POMELO_WRITER_ODT_H
#include "doc.h"

Document *odt_read(const char *path);
int odt_write(const Document *d, const char *path);

#endif
