#include "override.h"
#include "ft.h"
#include <windows.h>
#include <tchar.h>
#include "fteng.h"

#ifdef _DLL
#pragma comment(linker, "/nod:msvcprt.lib /nod:msvcprtd.lib")
#endif

#if 0
#define FREETYPE_REQCOUNTMAX	10
#define GC_TRACE				TRACE
#define FREETYPE_GC_COUNTER		128
#else
#define FREETYPE_REQCOUNTMAX	2048//Ĭ��4096,ÿ��������������ֽ�ˢ�»��潵���ڴ�ռ��
#define GC_TRACE				NOP_FUNCTION
#define FREETYPE_GC_COUNTER		1024//ˢ�»��������������
#endif

FreeTypeFontEngine* g_pFTEngine;
FT_Library     freetype_library;
FTC_Manager    cache_man;
FTC_CMapCache  cmap_cache;
FTC_ImageCache image_cache;
CTLSDCArray TLSDCArray;

int CALLBACK EnumFontCallBack(const LOGFONT *lplf, const TEXTMETRIC *lptm, DWORD /*FontType*/, LPARAM lParam)
{	
	LOGFONT * lf=(LOGFONT *)lParam;
	StringCchCopy(lf->lfFaceName, LF_FACESIZE, lplf->lfFaceName);
	lf->lfQuality=0x2d;	//magic number
	return 0;
}



bool GetFontLocalName(TCHAR* pszFontName, __out TCHAR* pszNameOut)	//�������ı��ػ�����,����ֵΪ�����Ƿ����
{
	LOGFONT lf = {0};
	TCHAR* ret;
	lf.lfQuality = 0x2d;
	if (!(ret = FontNameCache.Find((TCHAR*)pszFontName)))
	{
		StringCchCopy(lf.lfFaceName, LF_FACESIZE, pszFontName);
		lf.lfCharSet=DEFAULT_CHARSET;
		HDC dc=GetDC(NULL);
		lf.lfQuality=0;
		EnumFontFamiliesEx(dc, &lf, &EnumFontCallBack, (LPARAM)&lf, 0);
		ReleaseDC(NULL, dc);
		if (lf.lfQuality==0x2d)
			FontNameCache.Add((TCHAR*)pszFontName, lf.lfFaceName);
		ret=lf.lfFaceName;
	}
	StringCchCopy(pszNameOut, LF_FACESIZE, ret);
	return lf.lfQuality == 0x2d;
}

LOGFONTW* GetFontNameFromFile(LPCWSTR Filename)	//���һ�������ļ��ڰ�����������������s
{
	LOGFONTW* logfonts = NULL;
	DWORD bufsize=0;
	if (GetFontResourceInfo(Filename, &bufsize, NULL, 2))
	{
		logfonts = (LOGFONTW*)malloc(bufsize+1);
		if (GetFontResourceInfo(Filename, &bufsize, logfonts, 2))
		{
			((char*)logfonts)[bufsize]=0;
			return logfonts;
		}
		else
		{
			free(logfonts);
			return NULL;
		}
	}
	else
	{
		AddFontResourceW(Filename);
		if (GetFontResourceInfo(Filename, &bufsize, NULL, 2))
		{
			logfonts = (LOGFONTW*)malloc(bufsize+1);
			if (GetFontResourceInfo(Filename, &bufsize, logfonts, 2))
			{
				((char*)logfonts)[bufsize]=0;
				ORIG_RemoveFontResourceExW(Filename,0,NULL);
				return logfonts;
			}
			else
			{
				free(logfonts);
				ORIG_RemoveFontResourceExW(Filename,0,NULL);
				return NULL;
			}
		}
		ORIG_RemoveFontResourceExW(Filename,0,NULL);
		return NULL;
	}
}

template <class T>
struct GCCounterSortFunc : public std::binary_function<const T*, const T*, bool>
{
	bool operator()(const T* arg1, const T* arg2) const
	{
		const int cnt1 = arg1 ? arg1->GetMruCounter() : -1;
		const int cnt2 = arg2 ? arg2->GetMruCounter() : -1;
		return cnt1 > cnt2;
	}
};

struct DeleteCharFunc : public std::unary_function<FreeTypeCharData*&, void>
{
	void operator()(FreeTypeCharData*& arg) const
	{
		if (!arg)
			return;
		delete arg;
		arg = NULL;
	}
};

template <class T>
void CompactMap(T& pp, int count, int reduce)
{
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTCACHE);
	int reducecount = pp.size() - reduce;
	T::iterator it= pp.begin();
	for (int i=0;i<reducecount;i++) //ɾ������FREETYPE_GC_COUNTER֮��Ļ���
	{
		//it->second->Erase();
		delete it->second;
		pp.erase(it++);
	}
	return;
}

template <class T>
void Compact(T** pp, int count, int reduce)
{
	Assert(count >= 0);
	Assert(reduce > 0);
	if (!pp || !count || count < reduce) {
		return;
	}
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTCACHE);
	TRACE(_T("Compact(0x%p, %d, %d)\n"), pp, count, reduce);
	//GC���h�L
	//T::m_mrucounter�̍~���ɕ��Ԃ̂�
	//reduce�𒴂��镔�����폜����

	T** ppTemp = (T**)malloc(sizeof(T*) * count);
	if (!ppTemp) {
		return;
	}
	memcpy(ppTemp, pp, sizeof(T*) * count);

	std::sort(ppTemp, ppTemp + count, GCCounterSortFunc<T>());
	int i;
	for (i=0; i<reduce; i++) {
		if (!ppTemp[i])
			break;
		ppTemp[i]->ResetMruCounter();
	}

	//GC_TRACE(_T("GC:"));
	for (i=reduce; i<count; i++) {
		if (!ppTemp[i])
			break;
		//GC_TRACE(_T(" %wc"), ppTemp[i]->GetChar());
		ppTemp[i]->Erase();
	}
	//GC_TRACE(_T("\n"));
	free(ppTemp);
}

//FreeTypeCharData
FreeTypeCharData::FreeTypeCharData(FreeTypeCharData** ppCh, FreeTypeCharData** ppGl, WCHAR wch, UINT glyphindex, int width, int mru, int gdiWidth, int AAMode)
	: m_ppSelfGlyph(ppGl), m_glyphindex(glyphindex), m_width(width)
	, m_glyph(NULL), m_glyphMono(NULL), m_bmpSize(0), m_bmpMonoSize(0)
	, FreeTypeMruCounter(mru), m_gdiWidth(gdiWidth), m_AAMode(AAMode)
{
	g_pFTEngine->AddMemUsedObj(this);
	AddChar(ppCh);
#ifdef _DEBUG
	m_wch = wch;
#endif
}

FreeTypeCharData::~FreeTypeCharData()
{
	CharDataArray& arr = m_arrSelfChar;
	int n = arr.GetSize();
	while (n--) {
		FreeTypeCharData** pp = arr[n];
		Assert(*pp == this);
		*pp = NULL;
	}
	if(m_ppSelfGlyph) {
		Assert(*m_ppSelfGlyph == this);
		*m_ppSelfGlyph = NULL;
	}
	if(m_glyph){
		FT_Done_Ref_Glyph((FT_Referenced_Glyph*)&m_glyph);
	}
	if(m_glyphMono){
		FT_Done_Ref_Glyph((FT_Referenced_Glyph*)&m_glyphMono);
	}

	g_pFTEngine->SubMemUsed(m_bmpSize);
	g_pFTEngine->SubMemUsed(m_bmpMonoSize);
	g_pFTEngine->SubMemUsedObj(this);
}

void FreeTypeCharData::SetGlyph(FT_Render_Mode render_mode, FT_Referenced_BitmapGlyph glyph)
{
	const bool bMono = (render_mode == FT_RENDER_MODE_MONO);
	FT_Referenced_BitmapGlyph& gl = bMono ? m_glyphMono : m_glyph;
	if (gl) 
		FT_Done_Ref_Glyph((FT_Referenced_Glyph*)&gl);
	{
		FT_Glyph_Ref_Copy((FT_Referenced_Glyph)glyph, (FT_Referenced_Glyph*)&gl);
		if (gl) {
			int& size = bMono ? m_bmpMonoSize : m_bmpSize;
			size  = FT_Bitmap_CalcSize(gl->ft_glyph);
			size += sizeof(FT_BitmapGlyphRec);
			g_pFTEngine->AddMemUsed(size);
		}
	}
}


//FreeTypeFontCache
FreeTypeFontCache::FreeTypeFontCache(/*int px, int weight, bool italic, */int mru)
	: /*m_px(px), m_weight(weight), m_italic(italic), */m_active(false)
	, FreeTypeMruCounter(mru)
{
#ifdef _USE_ARRAY
	ZeroMemory(&m_tm, sizeof(TEXTMETRIC));
	ZeroMemory(m_chars, sizeof(m_chars));
	ZeroMemory(m_glyphs, sizeof(m_glyphs));
#else
	m_GlyphCache.clear();
#endif
	g_pFTEngine->AddMemUsedObj(this);
}

FreeTypeFontCache::~FreeTypeFontCache()
{
	Erase();
	g_pFTEngine->SubMemUsedObj(this);
}

void FreeTypeFontCache::Compact()
{
	//TRACE(_T("FreeTypeFontCache::Compact: %d > %d\n"), countof(m_chars), FREETYPE_GC_COUNTER);
	ResetGCCounter();
#ifdef _USE_ARRAY
	::Compact(m_glyphs, countof(m_glyphs), FREETYPE_GC_COUNTER);
#else
	::CompactMap(m_GlyphCache, m_GlyphCache.size(), FREETYPE_GC_COUNTER);
#endif
	//GlyphCache::const_iterator it=m_GlyphCache.begin();
}

void FreeTypeFontCache::Erase()
{
	m_active = false;
#ifdef _USE_ARRAY
	std::for_each(m_chars,  m_chars  + FT_MAX_CHARS, DeleteCharFunc());
	std::for_each(m_glyphs, m_glyphs + FT_MAX_CHARS, DeleteCharFunc());
#else
	GlyphCache::iterator it=m_GlyphCache.begin();
	for (;it!=m_GlyphCache.end();++it)
		delete it->second;
	m_GlyphCache.clear();
#endif
}

void FreeTypeFontCache::AddCharData(WCHAR wch, UINT glyphindex, int width, int gdiWidth, FT_Referenced_BitmapGlyph glyph, FT_Render_Mode render_mode, int AAMode)
{
	if (glyphindex & 0xffff0000 /*|| !g_ccbCache*/) {
		return;
	}
	if (AddIncrement() >= FREETYPE_REQCOUNTMAX) {	//��ѹ��������ѹ����ʧ������
		Compact();
	}

#ifdef _USE_ARRAY
	FreeTypeCharData** ppChar  = _GetChar(wch);
	if (*ppChar) {
		(*ppChar)->SetGlyph(render_mode, glyph);
		(*ppChar)->SetMruCounter(this);
		return;
	}
#else
	GlyphCache::iterator it=m_GlyphCache.find(wch);
	if (it!=m_GlyphCache.end())	//�ҵ��˾�����
	{
		FreeTypeCharData* ppChar  = it->second;
		if (ppChar) {
			ppChar->SetGlyph(render_mode, glyph);
			ppChar->SetMruCounter(this);
			ppChar->SetWidth(width);
			ppChar->SetGDIWidth(gdiWidth);
			return;
		}	
	}
#endif
	FreeTypeCharData* p = new FreeTypeCharData(/*ppChar*/NULL, NULL, wch, glyphindex, width, MruIncrement(), gdiWidth, AAMode);
	if (p == NULL) {
		return;
	}
	p->SetGlyph(render_mode, glyph);

#ifdef _USE_ARRAY
	*ppChar = p;
#else
	m_GlyphCache[wch]=p;
#endif

}

void FreeTypeFontCache::AddGlyphData(UINT glyphindex, int width, int gdiWidth, FT_Referenced_BitmapGlyph glyph, FT_Render_Mode render_mode, int AAMode)
{
	if (glyphindex & 0xffff0000 /*|| !g_ccbCache*/) {
		return;
	}
	//GC
	if (AddIncrement() >= FREETYPE_REQCOUNTMAX) {
		//TRACE(_T("Compact(0x%p)\n"), this);
		Compact();
	}

#ifdef _USE_ARRAY
	FreeTypeCharData** ppGlyph  = _GetGlyph(glyphindex);
	if (*ppGlyph) {
		(*ppGlyph)->SetGlyph(render_mode, glyph);
		(*ppGlyph)->SetMruCounter(this);
		return;
	}
#else
	GlyphCache::iterator it=m_GlyphCache.find(-(int)glyphindex);
	if (it!=m_GlyphCache.end())	//�ҵ��˾�����
	{
		FreeTypeCharData* ppChar  = it->second;
		if (ppChar) {
			(ppChar)->SetGlyph(render_mode, glyph);
			(ppChar)->SetMruCounter(this);
			ppChar->SetWidth(width);
			ppChar->SetGDIWidth(gdiWidth);
			return;
		}	
	}
#endif

	//�ǉ�(glyph�̂�)
	FreeTypeCharData* p = new FreeTypeCharData(NULL, /*ppGlyph*/NULL, 0, glyphindex, width, MruIncrement(), gdiWidth, AAMode);
	if (p == NULL) {
		return;
	}
	p->SetGlyph(render_mode, glyph);

#ifdef _USE_ARRAY
	*ppGlyph = p;
#else
	m_GlyphCache[-(int)glyphindex]=p;
#endif
}


//FreeTypeFontInfo
void FreeTypeFontInfo::Compact()
{
	//TRACE(_T("FreeTypeFontInfo::Compact: %d > %d\n"), m_cache.GetSize(), m_nMaxSizes);
	ResetGCCounter();
	::CompactMap(m_cache, m_cache.size(), m_nMaxSizes);
	CacheArray::const_iterator it=m_cache.begin();
	for (;it!=m_cache.end();++it)
		it->second->Deactive();
}

void FreeTypeFontInfo::Createlink()
{
	CFontFaceNamesEnumerator fn(m_hash.c_str(), m_nFontFamily);
	std::set<int> linkset;	//�������弯�ϣ���ֹ�ظ����ӣ�����Ч��
	linkset.insert(m_id);
	face_id_link[m_linknum] = (FTC_FaceID)m_id;
	ggo_link[m_linknum++] = m_ggoFont;	//��һ������һ�����Լ�������Ҫ��ȡ
	LOGFONT lf;
	BOOL IsSimSun = false;
	memset(&lf, 0, sizeof(LOGFONT));
	lf.lfCharSet=DEFAULT_CHARSET;
	const CGdippSettings* pSettings = CGdippSettings::GetInstance();
	for (fn.next() ; !fn.atend(); fn.next()) {	//������һ������
		//FreeTypeFontInfo* pfitemp = g_pFTEngine->FindFont(fn, m_weight, m_italic);
		//if (pfitemp && pfitemp->m_isSimSun)
		//	IsSimSun = true;
		if (!m_SimSunID)
			IsSimSun = (_wcsicmp(fn,L"����")==0 || _wcsicmp(fn,L"SimSun")==0);
		StringCchCopy(lf.lfFaceName, LF_FACESIZE, fn);
		pSettings->CopyForceFont(lf,lf);
		FreeTypeFontInfo* pfitemp = g_pFTEngine->FindFont(lf.lfFaceName, /*m_weight*/0, /*m_italic*/false);
		if (pfitemp && linkset.find(pfitemp->GetId())==linkset.end()) {
			linkset.insert(pfitemp->GetId());
			face_id_link[m_linknum] = (FTC_FaceID)pfitemp->GetId();
			ggo_link[m_linknum++] = pfitemp->GetGGOFont();
		if (!m_SimSunID && IsSimSun)
			m_SimSunID = (FTC_FaceID)pfitemp->GetId();
		}
	}
}

bool FreeTypeFontInfo::EmbeddedBmpExist(int px)
{
	if (px>=256 || px<0)
		return false;
	if (m_ebmps[px]!=-1)
		return !!m_ebmps[px];
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_MANAGER);
	FTC_ImageTypeRec imgtype={(FTC_FaceID)m_id, px, px, FT_LOAD_DEFAULT};	//����һ����ǰ��С��imagetype
	FT_Glyph temp_glyph=NULL;
	FT_UInt gindex = FTC_CMapCache_Lookup(cmap_cache, (FTC_FaceID)m_id, -1, FT_UInt32(L'0'));	//���0������ֵ
	FTC_ImageCache_Lookup(image_cache, &imgtype, gindex, &temp_glyph, NULL);
	if (temp_glyph && temp_glyph->format==FT_GLYPH_FORMAT_BITMAP)	//������Զ���0�ĵ���
		m_ebmps[px]=1;	//����ֺŴ��ڵ���
	else
	{
		gindex = FTC_CMapCache_Lookup(cmap_cache, (FTC_FaceID)m_id, -1, FT_UInt32(L'��'));	//���"��"������ֵ
		if (gindex)
			FTC_ImageCache_Lookup(image_cache, &imgtype, gindex, &temp_glyph, NULL);	//��ȡ���ġ��ĵ���
		if (temp_glyph && temp_glyph->format==FT_GLYPH_FORMAT_BITMAP)	//������Զ���0�ĵ���
			m_ebmps[px]=1;	//����ֺŴ��ڵ���
		else
			m_ebmps[px]=0;
	}
	return !!m_ebmps[px];
}

FreeTypeFontCache* FreeTypeFontInfo::GetCache(FTC_ScalerRec& scaler, const LOGFONT& lf)
{
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTCACHE);

	if (AddIncrement() > m_nMaxSizes) {	//��ѹ��
		Compact();
	}
	int weight = lf.lfWeight;
	weight = weight < FW_BOLD ? 0: 1/*FW_BOLD*/;
	const bool italic = !!lf.lfItalic;
	if (scaler.height>0xfff || scaler.width>0xfff || scaler.height<0 || scaler.width<0)	//�������岻��Ⱦ
		return NULL;
	FreeTypeFontCache* p = NULL;
	UINT hash=getCacheHash(scaler.height, weight, italic, lf.lfWidth ? scaler.width : 0);	//����hash
	CacheArray::iterator it=m_cache.find(hash); //Ѱ��cache
	if (it!=m_cache.end())//cache����
	{
		p = it->second;
		goto OK; //����cache
	}
	
	p = new FreeTypeFontCache(/*scaler.height, weight, italic,*/ MruIncrement());
	if (!p) {
		return NULL;
	}
	if (m_cache[hash]=p) {
		goto OK;
	}
	delete p;
	return NULL;

OK:
	Assert(p != NULL);
	if (p && p->Activate()) {
		DecIncrement();	//�ظ�ʹ���������ֵ
	}
	return p;
}


//FreeTypeFontEngine
void FreeTypeFontEngine::Compact()
{
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTENG);

	TRACE(_T("FreeTypeFontEngine::Compact: %d > %d\n"), m_mfontMap.size(), m_nMaxFaces);
	ResetGCCounter();
	//memset(m_arrFace, 0, sizeof(FT_Face)*m_nFaceCount);	//�������face���ˣ��ϵ�face�ᱻft�ͷŵ���������Ҫȫ�����»�ȡ
	//FontListArray& arr = m_arrFontList;
	//::Compact(arr.GetData(), arr.GetSize(), m_nMaxFaces);
}

BOOL FreeTypeFontEngine::RemoveFont(FreeTypeFontInfo* fontinfo)
{
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTMAP);
	{
		FontMap::const_iterator iter=m_mfontMap.begin();	//����fontmap
		while (iter!=m_mfontMap.end())
		{
			FreeTypeFontInfo* p = iter->second;
			if (p==fontinfo)
				m_mfontMap.erase(iter++);	//ɾ������
			else
				++iter;
		}
	}
	{
		FullNameMap::const_iterator iter=m_mfullMap.begin();	//����fullmap
		while (iter!=m_mfullMap.end())
		{
			FreeTypeFontInfo* p = iter->second;
			if (p==fontinfo)
				m_mfullMap.erase(iter++);	//ɾ������
			else
			{
				iter->second->UpdateFontSetting();
				++iter;
			}
		}
	}
	delete fontinfo;
	return true;
}

BOOL FreeTypeFontEngine::RemoveThisFont(FreeTypeFontInfo* fontinfo, LOGFONT* lg)
{
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTMAP);
	{
		FontMap::const_iterator iter=m_mfontMap.find(myfont(lg->lfFaceName, CalcBoldWeight(lg->lfWeight), lg->lfItalic));	//����fontmap
		if (iter!=m_mfontMap.end())
			m_mfontMap.erase(iter);	//ɾ������
	}
	{
		FullNameMap::const_iterator iter=m_mfullMap.find(fontinfo->GetFullName());	//����fullmap
		if (iter!=m_mfullMap.end())
			m_mfullMap.erase(iter);	//ɾ������
	}
	delete fontinfo;
	return true;
}

BOOL FreeTypeFontEngine::RemoveFont(LPCWSTR FontName)
{
	if (!FontName) return false;
	LOGFONTW* fontarray = GetFontNameFromFile(FontName);
	LOGFONTW* c_fontarray = fontarray;	//��¼ԭʼָ��
	if (!fontarray) return false;
	FTC_FaceID fid = NULL;
	BOOL bIsFontLoaded, bIsFontFileLoaded = false;
	COwnedCriticalSectionLock __lock2(2, COwnedCriticalSectionLock::OCS_DC);	//��ȡ����Ȩ������Ҫ����DC����ֹ���л�ͼ��������
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_MANAGER);
	while (*(char*)fontarray)
	{
		bIsFontLoaded = false;
		FreeTypeFontInfo* result = FindFont(fontarray->lfFaceName, fontarray->lfWeight, !!fontarray->lfItalic, false, &bIsFontLoaded);
		if (result)
		{
			fid = (FTC_FaceID)result->GetId();
			if (bIsFontLoaded)	//�������Ѿ���ʹ�ù�
			{
				RemoveFont(result);	//ö��������Ϣȫ��ɾ��
				bIsFontFileLoaded = true;	//���������ļ�Ҳ��ʹ�ù�
			}
			else
				RemoveThisFont(result, fontarray);
			CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTENG);
			FTC_Manager_RemoveFaceID(cache_man, fid);
			m_mfontList[(int)fid-1]=NULL;
		}
		fontarray++;
	}
	free(c_fontarray); //����ԭʼָ���ͷ�
	if (bIsFontFileLoaded)	//�������ļ���ʹ�ù�������Ҫ�������DC
	{
		CTLSDCArray::iterator iter = TLSDCArray.begin();
		while (iter!=TLSDCArray.end())
		{
			((CBitmapCache*)*iter)->~CBitmapCache();	//���������ʹ���е�DC
			++iter;
		}
	}
	return true;
}

static int FaceIDHolder = 0;
int GetFaceID(void)
{
	return (int)InterlockedIncrement((LONG volatile*)&FaceIDHolder);
}

void ReleaseFaceID(void)
{
	InterlockedDecrement((LONG volatile*)&FaceIDHolder);
}

FreeTypeFontInfo* FreeTypeFontEngine::AddFont(void* lpparams)
{
	FREETYPE_PARAMS* params = (FREETYPE_PARAMS*)lpparams;
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTENG);
	const LOGFONT& lplf = *params->lplf;
	if(lplf.lfFaceName == NULL || _tcslen(lplf.lfFaceName) == 0)
		return NULL;

	const CGdippSettings* pSettings = CGdippSettings::GetInstance();
	//const CFontSettings& fs = pSettings->FindIndividual(params->strFamilyName.c_str());
	FreeTypeFontInfo* pfi = new FreeTypeFontInfo(/*m_mfullMap.size() + 1*/GetFaceID(), lplf.lfFaceName, lplf.lfWeight, !!lplf.lfItalic, MruIncrement(), params->strFullName, params->strFamilyName);
	if (!pfi)
		return NULL;
	
	if (pfi->GetFullName().size()==0)	//������
		{
			delete pfi;
			ReleaseFaceID();
			return false;
		}
/*
	TCHAR buff[255]={0};
	if (params->strFamilyName.length()==8)
	{
		wsprintf(buff, L"Adding familiyname \"%s\" fullname \"%s\" weight %d\n\result: \"%s\"\n", params->strFamilyName.c_str(), params->strFullName.c_str(),
			params->lplf->lfWeight, pfi->GetFullName().c_str());
		Log(buff);
	}*/

	FullNameMap::const_iterator it = m_mfullMap.find(pfi->GetFullName());
	if (it!=m_mfullMap.end())	//���Ѿ����ڵ�������,ԭ���������滻ʹ��������ָ��һ������
	{
		delete pfi;	//ɾ���ղŴ���������
		ReleaseFaceID();
		pfi = it->second;//ָ��ԭ����
	}
	else
	{
		m_mfullMap[pfi->GetFullName()]=pfi;	//�����ڣ����ӵ�map��
		m_mfontList.push_back(pfi);
	}

	if (pfi->GetFullName()!=params->strFullName)	//���Ŀ���������ʵ���ƺ���Ҫ�����Ʋ�һ����˵���������滻
	{
		pfi->AddRef();	//�������ü���
		m_mfullMap[params->strFullName] = pfi;	//˫�����ã�ָ��ͬһ������
	}
		
	//bool ret = !!arr.Add(pfi);
	//weight = weight < FW_BOLD ? 0: FW_BOLD;
	myfont font(lplf.lfFaceName, lplf.lfWeight, !!params->otm->otmTextMetrics.tmItalic);
	/*
	FontMap::const_iterator it = m_mfontMap.find(font);
		if (it!=m_mfontMap.end())
		{
			it->second->Release();
		}*/
	
	m_mfontMap[font]=pfi;
	/*
	if (!ret) {
	delete pfi;
	return NULL;
	}*/


#ifdef _DEBUG
	{
		const CFontSettings& fs = pfi->GetFontSettings();
		TRACE(_T("AddFont: %s, %d, %d, %d, %d, %d, %d\n"), pfi->GetName(),
			fs.GetParam(0), fs.GetParam(1), fs.GetParam(2), fs.GetParam(3), fs.GetParam(4), fs.GetParam(5));
	}
#endif
	return pfi;
}

FreeTypeFontInfo* FreeTypeFontEngine::AddFont(LPCTSTR lpFaceName, int weight, bool italic, BOOL* bIsFontLoaded)
{
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTENG);
	if(lpFaceName == NULL || _tcslen(lpFaceName) == 0/* || FontExists(lpFaceName, weight, italic)*/)
		return NULL;

	//FontListArray& arr = m_arrFontList;

	const CGdippSettings* pSettings = CGdippSettings::GetInstance();
	//const CFontSettings& fs = pSettings->FindIndividual(lpFaceName);
	wstring dumy;
	//dumy.clear();
	FreeTypeFontInfo* pfi = new FreeTypeFontInfo(/*m_mfullMap.size() + 1*/GetFaceID(), lpFaceName, weight, italic, MruIncrement(), dumy, dumy);
	if (!pfi)
		return NULL;
	if (pfi->GetFullName().size()==0)	//������
	{
		delete pfi;
		ReleaseFaceID();
		return false;
	}

	FullNameMap::const_iterator it = m_mfullMap.find(pfi->GetFullName()); //�Ƿ�����map���д�����
	if (it!=m_mfullMap.end())	//�Ѿ�����
	{
		delete pfi;	//ɾ����������������
		ReleaseFaceID();
		pfi = it->second;	//ָ���Ѿ����ڵ�����
		if (bIsFontLoaded)
			*bIsFontLoaded = true;
		//pfi->AddRef();
	}
	else
	{
		m_mfullMap[pfi->GetFullName()]=pfi;	//�����ڣ����ӵ�map��
		m_mfontList.push_back(pfi);
		if (bIsFontLoaded)
			*bIsFontLoaded = false;
	}

	//bool ret = !!arr.Add(pfi);
	//weight = weight < FW_BOLD ? 0: FW_BOLD;
	myfont font(lpFaceName, weight, italic);
	m_mfontMap[font]=pfi;		//�����ڴ�Ҫmap��
/*
	if (!ret) {
		delete pfi;
		return NULL;
	}*/

	
#ifdef _DEBUG
	{
		const CFontSettings& fs = pfi->GetFontSettings();
		TRACE(_T("AddFont: %s, %d, %d, %d, %d, %d, %d\n"), pfi->GetName(),
				fs.GetParam(0), fs.GetParam(1), fs.GetParam(2), fs.GetParam(3), fs.GetParam(4), fs.GetParam(5));
	}
#endif
	return pfi;
}

int FreeTypeFontEngine::GetFontIdByName(LPCTSTR lpFaceName, int weight, bool italic)
{
	const FreeTypeFontInfo* pfi = FindFont(lpFaceName, weight, italic);
	return pfi ? pfi->GetId() : 0;
}

/*
LPCTSTR FreeTypeFontEngine::GetFontById(int faceid, int& weight, bool& italic)
{
	CCriticalSectionLock __lock;

	FreeTypeFontInfo** pp	= m_arrFontList.Begin();
	FreeTypeFontInfo** end	= m_arrFontList.End();
	for(; pp != end; ++pp) {
		FreeTypeFontInfo* p = *pp;
		if (p->GetId() == faceid) {
			p->SetMruCounter(this);
			weight = p->GetWeight();
			italic = p->IsItalic();
			return p->GetName();
		}
	}
	return NULL;
}
*/
FreeTypeFontInfo* FreeTypeFontEngine::FindFont(void* lpparams)
{
	FREETYPE_PARAMS* params = (FREETYPE_PARAMS*)lpparams;
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTMAP);
	FullNameMap::const_iterator iter=m_mfullMap.find(params->strFullName);
	if (iter!=m_mfullMap.end())
	{
		FreeTypeFontInfo* p = iter->second;
		if (p->GetFullName()!=params->strFullName)	//�����滻����
			return FindFont(params->lplf->lfFaceName, params->lplf->lfWeight, !!params->lplf->lfItalic);
		p->SetMruCounter(this);
		return p;
	}
	//m_bAddOnFind = true;
	return AddFont(params);
}

FreeTypeFontInfo* FreeTypeFontEngine::FindFont(LPCTSTR lpFaceName, int weight, bool italic, bool AddOnFind, BOOL* bIsFontLoaded)
{
/*
	if (m_bAddOnFind) 
	{
		m_bAddOnFind = false;
		return NULL;
	}*/

	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTMAP);
	weight = CalcBoldWeight(weight);
	myfont font(lpFaceName, weight, italic);
	FontMap::const_iterator iter=m_mfontMap.find(font);
	if (iter!=m_mfontMap.end())
	{
		FreeTypeFontInfo* p = iter->second;
		p->SetMruCounter(this);
/*
		TCHAR buff[255]={0};
		if (wcslen(lpFaceName)==8)
		{
			wsprintf(buff, L"Finding familiyname \"%s\" weight %d\n\tFound: \"%s\"\n", lpFaceName,
				weight, p->GetFullName().c_str());
			Log(buff);
		}*/
		if (bIsFontLoaded)
			*bIsFontLoaded = true;
		return p;
	}
	//m_bAddOnFind = true;
	return AddFont(lpFaceName, weight, italic, bIsFontLoaded);
}

FreeTypeFontInfo* FreeTypeFontEngine::FindFont(int faceid)
{
	CCriticalSectionLock __lock(CCriticalSectionLock::CS_FONTMAP);
	if (faceid>m_mfontList.size())
		return NULL;
	else
		return m_mfontList[faceid-1];	//����bug������
	/*
	FullNameMap::const_iterator iter=m_mfullMap.begin();
		for(; iter != m_mfullMap.end(); ++iter) {
			FreeTypeFontInfo* p = iter->second;
			if (p->GetId() == faceid) {
				p->SetMruCounter(this);
				return p;
			}
		}
		return NULL;*/
	
}


//FreeTypeSysFontData
// http://kikyou.info/diary/?200510#i10 ���Q�l�ɂ���
#include <freetype/tttables.h>	// FT_TRUETYPE_TABLES_H
#include <mmsystem.h>	//mmioFOURCC
#define TVP_TT_TABLE_ttcf	mmioFOURCC('t', 't', 'c', 'f')
#define TVP_TT_TABLE_name	mmioFOURCC('n', 'a', 'm', 'e')

// Windows�ɓo�^����Ă���t�H���g�̃o�C�i���f�[�^�𖼏̂���擾
FreeTypeSysFontData* FreeTypeSysFontData::CreateInstance(LPCTSTR name, int weight, bool italic)
{
	FreeTypeSysFontData* pData = new FreeTypeSysFontData;
	if (!pData) {
		return NULL;
	}
	if (!pData->Init(name, weight, italic)) {
		delete pData;
		return NULL;
	}
	return pData;
}

bool FreeTypeSysFontData::Init(LPCTSTR name, int weight, bool italic)
{
	const CGdippSettings* pSettings = CGdippSettings::GetInstance();
	void* pNameFromGDI		= NULL; // Windows ����擾���� name �^�O�̓��e
	void* pNameFromFreeType	= NULL; // FreeType ����擾���� name �^�O�̓��e
	HFONT hf = NULL;
	DWORD cbNameTable;
	DWORD cbFontData;
	int index;
	DWORD buf;
	FT_StreamRec& fsr = m_ftStream;
	m_name.assign(name);
	m_hdc = CreateCompatibleDC(NULL);
	if(m_hdc == NULL) {
		return false;
	}
	// ���O�ȊO�K��
	if (pSettings->FontSubstitutes() < SETTING_FONTSUBSTITUTE_ALL)
	{
		hf = CreateFont(
					12, 0, 0, 0, weight,
					italic, FALSE, FALSE,
					DEFAULT_CHARSET,
					OUT_DEFAULT_PRECIS,
					FONT_MAGIC_NUMBER,
					DEFAULT_QUALITY,
					DEFAULT_PITCH | FF_DONTCARE,
					name);
	}
	else
		hf = CreateFont(
					12, 0, 0, 0, weight,
					italic, FALSE, FALSE,
					DEFAULT_CHARSET,
					OUT_DEFAULT_PRECIS,
					CLIP_DEFAULT_PRECIS,
					DEFAULT_QUALITY,
					DEFAULT_PITCH | FF_DONTCARE,
					name);

	if(hf == NULL){
		return false;
	}

	m_hOldFont = SelectFont(m_hdc, hf);
	// �t�H���g�f�[�^������ꂻ�����`�F�b�N
	cbNameTable = ::GetFontData(m_hdc, TVP_TT_TABLE_name, 0, NULL, 0);
	if(cbNameTable == GDI_ERROR){
		goto ERROR_Init;
	}

	pNameFromGDI		= malloc(cbNameTable);
	if (!pNameFromGDI) {
		goto ERROR_Init;
	}
	pNameFromFreeType	= malloc(cbNameTable);
	if (!pNameFromFreeType) {
		goto ERROR_Init;
	}

	//- name �^�O�̓��e���������ɓǂݍ���
	if(GetFontData(m_hdc, TVP_TT_TABLE_name, 0, pNameFromGDI, cbNameTable) == GDI_ERROR){
		goto ERROR_Init;
	}

	// �t�H���g�T�C�Y�擾����
	cbFontData = ::GetFontData(m_hdc, TVP_TT_TABLE_ttcf, 0, &buf, 1);
	if(cbFontData == 1){
		// TTC �t�@�C�����Ǝv����
		cbFontData = ::GetFontData(m_hdc, TVP_TT_TABLE_ttcf, 0, NULL, 0);
		m_isTTC = true;
	}
	else{
		cbFontData = ::GetFontData(m_hdc, 0, 0, NULL, 0);
	}
	if(cbFontData == GDI_ERROR){
		// �G���[; GetFontData �ł͈����Ȃ�����
		goto ERROR_Init;
	}

	if (pSettings->UseMapping()) {
		HANDLE hmap = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_COMMIT | SEC_NOCACHE, 0, cbFontData, NULL);
		if (!hmap) {
			goto ERROR_Init;
		}
		m_pMapping = MapViewOfFile(hmap, FILE_MAP_ALL_ACCESS, 0, 0, cbFontData);
		m_dwSize = cbFontData;
		CloseHandle(hmap);

		if (m_pMapping) {
			::GetFontData(m_hdc, m_isTTC ? TVP_TT_TABLE_ttcf : 0, 0, m_pMapping, cbFontData);
		}
	}

	// FT_StreamRec �̊e�t�B�[���h�𖄂߂�
	fsr.base				= 0;
	fsr.size				= cbFontData;
	fsr.pos					= 0;
	fsr.descriptor.pointer	= this;
	fsr.pathname.pointer	= NULL;
	fsr.read				= IoFunc;
	fsr.close				= CloseFunc;

	index = 0;
	m_locked = true;
	if(!OpenFaceByIndex(index)){
		goto ERROR_Init;
	}

	for(;;) {
		// FreeType ����Aname �^�O�̃T�C�Y���擾����
		FT_ULong length = 0;
		FT_Error err = FT_Load_Sfnt_Table(m_ftFace, TTAG_name, 0, NULL, &length);
		if(err){
			goto ERROR_Init;
		}

		// FreeType ���瓾�� name �^�O�̒����� Windows ���瓾�������Ɣ�r
		if(length == cbNameTable){
			// FreeType ���� name �^�O���擾
			err = FT_Load_Sfnt_Table(m_ftFace, TTAG_name, 0, (unsigned char*)pNameFromFreeType, &length);
			if(err){
				goto ERROR_Init;
			}
			// FreeType ����ǂݍ��� name �^�O�̓��e�ƁAWindows ����ǂݍ���
			// name �^�O�̓��e���r����B
			// ��v���Ă���΂��� index �̃t�H���g���g���B
			if(!memcmp(pNameFromGDI, pNameFromFreeType, cbNameTable)){
				// ��v����
				// face �͊J�����܂�
				break; // ���[�v�𔲂���
			}
		}

		// ��v���Ȃ�����
		// �C���f�b�N�X������₵�A���� face ���J��
		index ++;

		if(!OpenFaceByIndex(index)){
			// ��v���� face ���Ȃ��܂� �C���f�b�N�X���͈͂𒴂����ƌ�����
			// index �� 0 �ɐݒ肵�Ă��� index ���J���A���[�v�𔲂���
			index = 0;
			if(!OpenFaceByIndex(index)){
				goto ERROR_Init;
			}
			break;
		}
	}

	free(pNameFromGDI);
	free(pNameFromFreeType);
	m_locked = false;
	return true;

ERROR_Init:
	m_locked = false;
	if (hf) {
		SelectFont(m_hdc, m_hOldFont);
		DeleteFont(hf);
		m_hOldFont = NULL;
	}
	free(pNameFromGDI);
	free(pNameFromFreeType);
	return false;
}

unsigned long FreeTypeSysFontData::IoFunc(
			FT_Stream		stream,
			unsigned long	offset,
			unsigned char*	buffer,
			unsigned long	count )
{
	if(count == 0){
		return 0;
	}

	FreeTypeSysFontData * pThis = reinterpret_cast<FreeTypeSysFontData*>(stream->descriptor.pointer);
	Assert(pThis != NULL);

	DWORD result = 0;
	if (pThis->m_pMapping) {
		result = Min(pThis->m_dwSize - offset, count);
		memcpy(buffer, (const BYTE*)pThis->m_pMapping + offset, result);
	} else {
		result = ::GetFontData(pThis->m_hdc, pThis->m_isTTC ? TVP_TT_TABLE_ttcf : 0, offset, buffer, count);
		if(result == GDI_ERROR) {
			// �G���[
			return 0;
		}
	}
	return result;
}

void FreeTypeSysFontData::CloseFunc(FT_Stream stream)
{
	FreeTypeSysFontData * pThis = reinterpret_cast<FreeTypeSysFontData*>(stream->descriptor.pointer);
	Assert(pThis != NULL);

	if(!pThis->m_locked)
		delete pThis;
}

bool FreeTypeSysFontData::OpenFaceByIndex(int index)
{
	if(m_ftFace) {
		FT_Done_Face(m_ftFace);
		m_ftFace = NULL;
	}

	FT_Open_Args args = { 0 };
	args.flags		= FT_OPEN_STREAM;
	args.stream		= &m_ftStream;

	// FreeType �ň����邩�H
	return (FT_Open_Face(freetype_library, &args, index, &m_ftFace) == 0);
}