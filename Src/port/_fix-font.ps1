# Fix FontPlatformData.cpp: robust scaled-font creation + FT face setup.
$ErrorActionPreference = 'Stop'
$f = 'C:\Users\media\source\repos\Vibe\Apotheosis\WebKit\Source\WebCore\platform\graphics\FontPlatformData.cpp'
$text = [System.IO.File]::ReadAllText($f)
$crlf = "`r`n"

# ---- Replacement A: FT face setup (size + charmap) ----
$oldA = @(
'                if (FT_New_Memory_Face(ftLibrary, bytes.data(), static_cast<FT_Long>(bytes.size()), 0, &ftFace) == 0) {',
'                    // cairo_ft_font_face_create_for_ft_face takes ownership of ftFace',
'                    // (owns_face=TRUE) and calls FT_Done_Face on release.',
'                    sFace = cairo_ft_font_face_create_for_ft_face(ftFace, 0);'
) -join $crlf
$newA = @(
'                if (FT_New_Memory_Face(ftLibrary, bytes.data(), static_cast<FT_Long>(bytes.size()), 0, &ftFace) == 0) {',
'                    // Apotheosis: ensure the face has a valid size and a usable Unicode',
'                    // charmap - the cairo FT backend needs both to scale the face.',
'                    FT_Set_Pixel_Sizes(ftFace, 0, 16);',
'                    FT_Select_Charmap(ftFace, FT_ENCODING_UNICODE);',
'                    // cairo_ft_font_face_create_for_ft_face takes ownership of ftFace',
'                    // (owns_face=TRUE) and calls FT_Done_Face on release.',
'                    sFace = cairo_ft_font_face_create_for_ft_face(ftFace, 0);'
) -join $crlf

# ---- Replacement B: ctor WK_WINUWP block ----
$oldB = @(
'#if defined(WK_WINUWP)',
'    // Apotheosis: Create a default Cairo scaled font so m_scaledFont is never null.',
'    // Prefer the bundled-TTF FreeType face (real glyphs); fall back to the toy face.',
'    {',
'        cairo_font_face_t* face = apotheosisBundledFontFace();',
'        bool ownsFace = false;',
'        if (!face) {',
'            face = cairo_toy_font_face_create("sans-serif",',
'                CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);',
'            ownsFace = true;',
'        }',
'        if (face && cairo_font_face_status(face) == CAIRO_STATUS_SUCCESS) {',
'            cairo_matrix_t fontMatrix, ctm;',
'            float actualSize = size ? size : 12.0f;',
'            cairo_matrix_init_scale(&fontMatrix, actualSize, actualSize);',
'            cairo_matrix_init_identity(&ctm);',
'            cairo_font_options_t* opts = cairo_font_options_create();',
'            cairo_font_options_set_hint_style(opts, CAIRO_HINT_STYLE_NONE);',
'            m_scaledFont = adoptRef(cairo_scaled_font_create(face, &fontMatrix, &ctm, opts));',
'            cairo_font_options_destroy(opts);',
'        }',
'        if (ownsFace && face)',
'            cairo_font_face_destroy(face);',
'    }',
'#endif'
) -join $crlf
$newB = @(
'#if defined(WK_WINUWP)',
'    // Apotheosis: Create a default Cairo scaled font so m_scaledFont is never null.',
'    // Prefer the bundled-TTF FreeType face (real glyphs); fall back to the toy face.',
'    {',
'        float actualSize = size ? size : 12.0f;',
'        if (!(actualSize > 0.0f && actualSize < 1.0e6f))',
'            actualSize = 12.0f;',
'',
'        cairo_font_face_t* face = apotheosisBundledFontFace();',
'        bool ownsFace = false;',
'        if (!face) {',
'            face = cairo_toy_font_face_create("sans-serif",',
'                CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);',
'            ownsFace = true;',
'        }',
'        if (face && cairo_font_face_status(face) == CAIRO_STATUS_SUCCESS) {',
'            cairo_matrix_t fontMatrix, ctm;',
'            cairo_matrix_init_scale(&fontMatrix, actualSize, actualSize);',
'            cairo_matrix_init_identity(&ctm);',
'            cairo_font_options_t* opts = cairo_font_options_create();',
'            cairo_font_options_set_hint_style(opts, CAIRO_HINT_STYLE_NONE);',
'            m_scaledFont = adoptRef(cairo_scaled_font_create(face, &fontMatrix, &ctm, opts));',
'            cairo_font_options_destroy(opts);',
'        }',
'        if (ownsFace && face)',
'            cairo_font_face_destroy(face);',
'',
'        // Apotheosis: the FT backend can return an in-error scaled font (e.g. when the',
'        // face cannot be locked/scaled). An error scaled font must never be used - it',
'        // crashes on the first layout call. Rebuild with the guaranteed-safe toy face.',
'        if (!m_scaledFont || cairo_scaled_font_status(m_scaledFont.get()) != CAIRO_STATUS_SUCCESS) {',
'            m_scaledFont = nullptr;',
'            cairo_matrix_t fontMatrix, ctm;',
'            cairo_matrix_init_scale(&fontMatrix, actualSize, actualSize);',
'            cairo_matrix_init_identity(&ctm);',
'            cairo_font_face_t* toy = cairo_toy_font_face_create("sans-serif",',
'                CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);',
'            cairo_font_options_t* opts = cairo_font_options_create();',
'            cairo_font_options_set_hint_style(opts, CAIRO_HINT_STYLE_NONE);',
'            m_scaledFont = adoptRef(cairo_scaled_font_create(toy, &fontMatrix, &ctm, opts));',
'            cairo_font_options_destroy(opts);',
'            cairo_font_face_destroy(toy);',
'        }',
'    }',
'#endif'
) -join $crlf

$countA = ([regex]::Matches($text, [regex]::Escape($oldA))).Count
$countB = ([regex]::Matches($text, [regex]::Escape($oldB))).Count
Write-Host "match A: $countA, match B: $countB"
if ($countA -ne 1 -or $countB -ne 1) { Write-Host 'ABORT: expected exactly one match each'; exit 1 }

$text = $text.Replace($oldA, $newA).Replace($oldB, $newB)
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($f, $text, $utf8)
Write-Host 'OK: written'
