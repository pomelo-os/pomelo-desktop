/* SPDX-License-Identifier: GPL-3.0-or-later */
/* xml.h — 給 .docx（OOXML）／.odt（ODF）用的最小 XML 剖析器。
 *
 * 不處理 DTD、XInclude、命名空間重新對應（OOXML／ODF 的前綴固定用 w:、text:、style: 這些，
 * 直接把完整標籤名稱如 "w:p" 當字串比對即可，不需要真的解析 xmlns）。
 * 支援：元素、屬性、文字內容、CDATA、註解（略過）、五種內建實體與 &#NNN;／&#xHH; 數字字元參照。
 */
#ifndef POMELO_WRITER_XML_H
#define POMELO_WRITER_XML_H
#include <stddef.h>

typedef struct XmlAttr {
	char *name, *value;
	struct XmlAttr *next;
} XmlAttr;

typedef struct XmlNode {
	char *tag;                  /* NULL 代表這是純文字節點 */
	char *text;                 /* tag 為 NULL 時的文字內容（已解過實體） */
	XmlAttr *attrs;
	struct XmlNode *children, *next;   /* children：第一個子節點；next：下一個兄弟節點 */
} XmlNode;

/* 剖析一段 XML（不需要 NUL 結尾，給長度）；失敗回傳 NULL。回傳的是文件的根元素
 * （略過 <?xml ...?>、DOCTYPE、最外層的註解／空白）。 */
XmlNode *xml_parse(const char *data, size_t len);
void xml_free(XmlNode *n);

/* 找第一個／逐一找符合標籤名稱的直接子節點（不遞迴） */
XmlNode *xml_child(const XmlNode *n, const char *tag);
XmlNode *xml_next(const XmlNode *n, const char *tag);   /* 從 n 的下一個兄弟開始找 */
const char *xml_attr(const XmlNode *n, const char *name);
/* 把這個元素底下所有文字子節點（含子元素裡的）串起來，呼叫者要 free */
char *xml_text(const XmlNode *n);

/* ---- 寫入（只需要串流式輸出，不需要先建樹） ---- */
typedef struct XmlWriter XmlWriter;
XmlWriter *xml_writer_new(void);
void xml_open(XmlWriter *w, const char *tag);
void xml_attr_set(XmlWriter *w, const char *name, const char *value);   /* 只能在 xml_open 後、第一個子節點或文字前呼叫 */
void xml_text_out(XmlWriter *w, const char *text);   /* 會自動跳脫 & < > ，保留純文字（不是另一個元素） */
void xml_close(XmlWriter *w);         /* 關閉最近一層還沒關的元素 */
char *xml_writer_finish(XmlWriter *w, size_t *outlen);   /* 補關所有還沒關的元素；呼叫者要 free */
void xml_writer_free(XmlWriter *w);

#endif
