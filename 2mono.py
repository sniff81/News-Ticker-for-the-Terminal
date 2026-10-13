import sys
import os
from fontTools.ttLib import TTFont

def make_custom_monospace(target_path, output_path, letter_spacing=0):
    print(f"Processing target font: {target_path}")
    font = TTFont(target_path)
    
    hmtx = font['hmtx']
    glyf = font['glyf'] if 'glyf' in font else None

    if not glyf:
        print("Error: This script requires a TrueType (.ttf) font with a standard glyf table.")
        sys.exit(1)

    # 1. FIND STANDARD GLYPH WIDTH (Ignoring oversized outliers like emdash or wide ligatures)
    max_physical_width = 0
    widest_glyph_name = ""

    for glyph_name in hmtx.metrics.keys():
        if glyph_name in glyf:
            glyph = glyf[glyph_name]
            if hasattr(glyph, 'recalcBounds'):
                glyph.recalcBounds(glyf)
            
            x_min = getattr(glyph, 'xMin', None)
            x_max = getattr(glyph, 'xMax', None)
            
            if x_min is not None and x_max is not None:
                glyph_width = x_max - x_min
                # Ignore spaces, emdashes, and oversized symbols (> 1400 units) 
                # so standard letters dictate the monospaced grid width.
                if 0 < glyph_width < 1400:
                    if glyph_width > max_physical_width:
                        max_physical_width = glyph_width
                        widest_glyph_name = glyph_name

    # Fallback if no standard width found
    if max_physical_width == 0:
        max_physical_width = 1024

    # 2. CALCULATE UNIFORM CELL WIDTH WITH SPACING (Safely clamped to >= 0)
    target_width = max(0, max_physical_width + letter_spacing)
    print(f"Monospace cell width set to: {target_width} units (Base glyph '{widest_glyph_name}' width {max_physical_width} + spacing offset {letter_spacing})\n")

    # 3. FORCE THE MONOSPACE GRID AND CENTER ALL GLYPHS
    for glyph_name in hmtx.metrics.keys():
        advance_width, lsb = hmtx[glyph_name]
        
        if glyph_name in glyf:
            glyph = glyf[glyph_name]
            x_min = getattr(glyph, 'xMin', None)
            x_max = getattr(glyph, 'xMax', None)
            
            if x_min is not None and x_max is not None:
                glyph_width = x_max - x_min
                
                if glyph_width > 0:
                    new_lsb = int((target_width - glyph_width) / 2)
                    shift_delta = new_lsb - x_min
                    
                    if shift_delta != 0 and hasattr(glyph, 'move'):
                        glyph.move(shift_delta, 0)
                    
                    lsb = new_lsb

        hmtx[glyph_name] = (int(target_width), int(lsb))

    # 4. PURGE OPEN-TYPE TABLES TO STRIP LIGATURES AND PADDING RULES
    tables_to_purge = ['GSUB', 'GPOS', 'GDEF', 'kern']
    for table in tables_to_purge:
        if table in font:
            del font[table]
            print(f"  -> Successfully purged layout table: {table}")

    # 5. WRITE MONOSPACE SYSTEM FLAGS
    if 'OS/2' in font:
        font['OS/2'].panose.bProportion = 11  # Monospace Panose Flag
        font['OS/2'].xAvgCharWidth = int(target_width)
        font['OS/2'].fsSelection |= (1 << 9)  

    if 'post' in font:
        font['post'].isFixedPitch = 1  # Fixed Pitch Active
        font['post'].minMemIn = 0
        font['post'].maxMemIn = 0

    font.save(output_path)
    print(f"\nSuccess! Custom monospaced font saved to: {output_path}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python force_mono.py <your_target.ttf> [letter_spacing]")
        print("Example: python force_mono.py ndot-57-aligned.ttf -50")
        sys.exit(1)
        
    tgt = sys.argv[1]
    
    spacing = 0
    if len(sys.argv) > 2:
        try:
            spacing = int(sys.argv[2])
        except ValueError:
            print(f"Error: Spacing value '{sys.argv[2]}' must be an integer.")
            sys.exit(1)
    
    out = os.path.splitext(tgt)[0] + "_custom_mono.ttf"
    make_custom_monospace(tgt, out, letter_spacing=spacing)
