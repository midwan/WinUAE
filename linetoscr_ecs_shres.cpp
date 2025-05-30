/*
* UAE - The portable Amiga emulator.
*
* This file was generated by genlinetoscr. Don't edit.
*/

static void lts_ecs_shres_dhires(void)
{
	while (denise_cck < denise_endcycle) {
		do_denise_cck(denise_linecnt, denise_startpos, denise_cck);
		if (lts_changed) return;
		for (int h = 0; h < 2; h++) {
			if (h) {
				denise_hcounter_next = denise_hcounter_new;
			}
			int cnt = denise_hcounter << 2;
			uae_u8 pix0 = 0;
			uae_u32 dpix_val0 = BLANK_COLOR;
			uae_u8 pix1 = 0;
			uae_u32 dpix_val1 = BLANK_COLOR;
			uae_u8 pix2 = 0;
			uae_u32 dpix_val2 = BLANK_COLOR;
			uae_u8 pix3 = 0;
			uae_u32 dpix_val3 = BLANK_COLOR;
			bool shifted = false;
			checkhorizontal1_ecs(denise_hcounter, denise_hcounter_next, h);
			if (!denise_blank_active) {
				dpix_val0 = bordercolor_ecs_shres;
				dpix_val1 = bordercolor_ecs_shres;
				dpix_val2 = bordercolor_ecs_shres;
				dpix_val3 = bordercolor_ecs_shres;
				if (denise_hdiw && bpl1dat_trigger) {
					shifted = true;
					pix0 = getbpl2();
					shiftbpl2();
					pix1 = getbpl2();
					shiftbpl2();
					pix2 = getbpl2();
					shiftbpl2();
					pix3 = getbpl2();
					shiftbpl2();
					get_shres_pix(pix0, pix1, &dpix_val0, &dpix_val1);
					get_shres_pix(pix2, pix3, &dpix_val2, &dpix_val3);
				}
			}
			uae_u32 sv0 = 0;
			if (denise_spr_nr_armeds) {
				uae_u32 svt;
				svt = denise_render_sprites_ecs_shres();
				if (!denise_sprite_blank_active && !sprites_hidden) {
					sv0 = svt;
				}
			}
			matchsprites_ecs_shres(cnt + 0);
			uae_u32 sv1 = 0;
			if (denise_spr_nr_armeds) {
				uae_u32 svt;
				svt = denise_render_sprites_ecs_shres();
				if (!denise_sprite_blank_active && !sprites_hidden) {
					sv1 = svt;
				}
			}
			matchsprites_ecs_shres(cnt + 2);
			if (sv0 || sv1) {
				get_shres_spr_pix(sv0, sv1, &dpix_val0, &dpix_val2);
				if (sv0) {
					dpix_val1 = dpix_val0;
				}
				if (sv1) {
					dpix_val3 = dpix_val2;
				}
			}
			if (denise_pixtotal >= 0 && denise_pixtotal < denise_pixtotal_max) {
				#ifdef DEBUGGER
				if (decode_specials_debug) {
					dpix_val0 = decode_denise_specials_debug(dpix_val0, cnt + 0);
				}
				#endif
				*buf1++ = dpix_val0;
				*buf2++ = dpix_val0;
				#ifdef DEBUGGER
				if (decode_specials_debug) {
					dpix_val2 = decode_denise_specials_debug(dpix_val2, cnt + 2);
				}
				#endif
				*buf1++ = dpix_val2;
				*buf2++ = dpix_val2;
			}
			if (!shifted) {
				shiftbpl2();
				shiftbpl2();
				shiftbpl2();
				shiftbpl2();
			}
			if (bpldat_copy[0] && (denise_hcounter_cmp & 3) == bplcon1_shift[0]) { 
				copybpl2();
			}
			internal_pixel_cnt += 4;
			sprites_hidden = sprites_hidden2;
			#ifdef DEBUGGER
			*debug_dma_dhpos_odd = denise_hcounter;
			#endif
			denise_hcounter_cmp++;
			denise_hcounter_cmp &= 511;
			denise_hcounter++;
			denise_hcounter &= 511;
			denise_hcounter_next++;
			denise_hcounter_next &= 511;
		}
		denise_pixtotal++;
		if (denise_pixtotal == 0) {
			internal_pixel_start_cnt = internal_pixel_cnt;
		}
		denise_hcounter = denise_hcounter_new;
		if (denise_accurate_mode) {
			denise_hcounter_cmp = denise_hcounter_new;
		}
		denise_cck++;
	}
}
static void lts_ecs_shres_dhires_filtered(void)
{
	while (denise_cck < denise_endcycle) {
		do_denise_cck(denise_linecnt, denise_startpos, denise_cck);
		if (lts_changed) return;
		for (int h = 0; h < 2; h++) {
			if (h) {
				denise_hcounter_next = denise_hcounter_new;
			}
			int cnt = denise_hcounter << 2;
			uae_u8 pix0 = 0;
			uae_u32 dpix_val0 = BLANK_COLOR;
			uae_u8 pix1 = 0;
			uae_u32 dpix_val1 = BLANK_COLOR;
			uae_u8 pix2 = 0;
			uae_u32 dpix_val2 = BLANK_COLOR;
			uae_u8 pix3 = 0;
			uae_u32 dpix_val3 = BLANK_COLOR;
			bool shifted = false;
			checkhorizontal1_ecs(denise_hcounter, denise_hcounter_next, h);
			if (!denise_blank_active) {
				dpix_val0 = bordercolor_ecs_shres;
				dpix_val1 = bordercolor_ecs_shres;
				dpix_val2 = bordercolor_ecs_shres;
				dpix_val3 = bordercolor_ecs_shres;
				if (denise_hdiw && bpl1dat_trigger) {
					shifted = true;
					pix0 = getbpl2();
					shiftbpl2();
					pix1 = getbpl2();
					shiftbpl2();
					pix2 = getbpl2();
					shiftbpl2();
					pix3 = getbpl2();
					shiftbpl2();
					get_shres_pix(pix0, pix1, &dpix_val0, &dpix_val1);
					get_shres_pix(pix2, pix3, &dpix_val2, &dpix_val3);
				}
			}
			uae_u32 sv0 = 0;
			if (denise_spr_nr_armeds) {
				uae_u32 svt;
				svt = denise_render_sprites_ecs_shres();
				if (!denise_sprite_blank_active && !sprites_hidden) {
					sv0 = svt;
				}
			}
			matchsprites_ecs_shres(cnt + 0);
			uae_u32 sv1 = 0;
			if (denise_spr_nr_armeds) {
				uae_u32 svt;
				svt = denise_render_sprites_ecs_shres();
				if (!denise_sprite_blank_active && !sprites_hidden) {
					sv1 = svt;
				}
			}
			matchsprites_ecs_shres(cnt + 2);
			if (sv0 || sv1) {
				get_shres_spr_pix(sv0, sv1, &dpix_val0, &dpix_val2);
				if (sv0) {
					dpix_val1 = dpix_val0;
				}
				if (sv1) {
					dpix_val3 = dpix_val2;
				}
			}
			if (denise_pixtotal >= 0 && denise_pixtotal < denise_pixtotal_max) {
				dpix_val0 = filter_pixel(dpix_val0, dpix_val1);
				dpix_val2 = filter_pixel(dpix_val2, dpix_val3);
				#ifdef DEBUGGER
				if (decode_specials_debug) {
					dpix_val0 = decode_denise_specials_debug(dpix_val0, cnt + 0);
				}
				#endif
				*buf1++ = dpix_val0;
				*buf2++ = dpix_val0;
				#ifdef DEBUGGER
				if (decode_specials_debug) {
					dpix_val2 = decode_denise_specials_debug(dpix_val2, cnt + 2);
				}
				#endif
				*buf1++ = dpix_val2;
				*buf2++ = dpix_val2;
			}
			if (!shifted) {
				shiftbpl2();
				shiftbpl2();
				shiftbpl2();
				shiftbpl2();
			}
			if (bpldat_copy[0] && (denise_hcounter_cmp & 3) == bplcon1_shift[0]) { 
				copybpl2();
			}
			internal_pixel_cnt += 4;
			sprites_hidden = sprites_hidden2;
			#ifdef DEBUGGER
			*debug_dma_dhpos_odd = denise_hcounter;
			#endif
			denise_hcounter_cmp++;
			denise_hcounter_cmp &= 511;
			denise_hcounter++;
			denise_hcounter &= 511;
			denise_hcounter_next++;
			denise_hcounter_next &= 511;
		}
		denise_pixtotal++;
		if (denise_pixtotal == 0) {
			internal_pixel_start_cnt = internal_pixel_cnt;
		}
		denise_hcounter = denise_hcounter_new;
		if (denise_accurate_mode) {
			denise_hcounter_cmp = denise_hcounter_new;
		}
		denise_cck++;
	}
}
static void lts_ecs_shres_dshres(void)
{
	while (denise_cck < denise_endcycle) {
		do_denise_cck(denise_linecnt, denise_startpos, denise_cck);
		if (lts_changed) return;
		for (int h = 0; h < 2; h++) {
			if (h) {
				denise_hcounter_next = denise_hcounter_new;
			}
			int cnt = denise_hcounter << 2;
			uae_u8 pix0 = 0;
			uae_u32 dpix_val0 = BLANK_COLOR;
			uae_u8 pix1 = 0;
			uae_u32 dpix_val1 = BLANK_COLOR;
			uae_u8 pix2 = 0;
			uae_u32 dpix_val2 = BLANK_COLOR;
			uae_u8 pix3 = 0;
			uae_u32 dpix_val3 = BLANK_COLOR;
			bool shifted = false;
			checkhorizontal1_ecs(denise_hcounter, denise_hcounter_next, h);
			if (!denise_blank_active) {
				dpix_val0 = bordercolor_ecs_shres;
				dpix_val1 = bordercolor_ecs_shres;
				dpix_val2 = bordercolor_ecs_shres;
				dpix_val3 = bordercolor_ecs_shres;
				if (denise_hdiw && bpl1dat_trigger) {
					shifted = true;
					pix0 = getbpl2();
					shiftbpl2();
					pix1 = getbpl2();
					shiftbpl2();
					pix2 = getbpl2();
					shiftbpl2();
					pix3 = getbpl2();
					shiftbpl2();
					get_shres_pix(pix0, pix1, &dpix_val0, &dpix_val1);
					get_shres_pix(pix2, pix3, &dpix_val2, &dpix_val3);
				}
			}
			uae_u32 sv0 = 0;
			if (denise_spr_nr_armeds) {
				uae_u32 svt;
				svt = denise_render_sprites_ecs_shres();
				if (!denise_sprite_blank_active && !sprites_hidden) {
					sv0 = svt;
				}
			}
			matchsprites_ecs_shres(cnt + 0);
			uae_u32 sv1 = 0;
			if (denise_spr_nr_armeds) {
				uae_u32 svt;
				svt = denise_render_sprites_ecs_shres();
				if (!denise_sprite_blank_active && !sprites_hidden) {
					sv1 = svt;
				}
			}
			matchsprites_ecs_shres(cnt + 2);
			if (sv0 || sv1) {
				get_shres_spr_pix(sv0, sv1, &dpix_val0, &dpix_val2);
				if (sv0) {
					dpix_val1 = dpix_val0;
				}
				if (sv1) {
					dpix_val3 = dpix_val2;
				}
			}
			if (denise_pixtotal >= 0 && denise_pixtotal < denise_pixtotal_max) {
				#ifdef DEBUGGER
				if (decode_specials_debug) {
					dpix_val0 = decode_denise_specials_debug(dpix_val0, cnt + 0);
				}
				#endif
				*buf1++ = dpix_val0;
				*buf2++ = dpix_val0;
				#ifdef DEBUGGER
				if (decode_specials_debug) {
					dpix_val1 = decode_denise_specials_debug(dpix_val1, cnt + 1);
				}
				#endif
				*buf1++ = dpix_val1;
				*buf2++ = dpix_val1;
				#ifdef DEBUGGER
				if (decode_specials_debug) {
					dpix_val2 = decode_denise_specials_debug(dpix_val2, cnt + 2);
				}
				#endif
				*buf1++ = dpix_val2;
				*buf2++ = dpix_val2;
				#ifdef DEBUGGER
				if (decode_specials_debug) {
					dpix_val3 = decode_denise_specials_debug(dpix_val3, cnt + 3);
				}
				#endif
				*buf1++ = dpix_val3;
				*buf2++ = dpix_val3;
			}
			if (!shifted) {
				shiftbpl2();
				shiftbpl2();
				shiftbpl2();
				shiftbpl2();
			}
			if (bpldat_copy[0] && (denise_hcounter_cmp & 3) == bplcon1_shift[0]) { 
				copybpl2();
			}
			internal_pixel_cnt += 4;
			sprites_hidden = sprites_hidden2;
			#ifdef DEBUGGER
			*debug_dma_dhpos_odd = denise_hcounter;
			#endif
			denise_hcounter_cmp++;
			denise_hcounter_cmp &= 511;
			denise_hcounter++;
			denise_hcounter &= 511;
			denise_hcounter_next++;
			denise_hcounter_next &= 511;
		}
		denise_pixtotal++;
		if (denise_pixtotal == 0) {
			internal_pixel_start_cnt = internal_pixel_cnt;
		}
		denise_hcounter = denise_hcounter_new;
		if (denise_accurate_mode) {
			denise_hcounter_cmp = denise_hcounter_new;
		}
		denise_cck++;
	}
}
static LINETOSRC_FUNC linetoscr_ecs_shres_funcs[] = {
	lts_ecs_shres_dhires,
	lts_ecs_shres_dhires,
	lts_ecs_shres_dshres,
	NULL
};
static LINETOSRC_FUNC linetoscr_ecs_shres_funcs_filtered[] = {
	NULL
};
static void lts_ecs_shres_dhires_genlock(void)
{
	while (denise_cck < denise_endcycle) {
		do_denise_cck(denise_linecnt, denise_startpos, denise_cck);
		if (lts_changed) return;
		for (int h = 0; h < 2; h++) {
			if (h) {
				denise_hcounter_next = denise_hcounter_new;
			}
			int cnt = denise_hcounter << 2;
			uae_u8 pix0 = 0;
			uae_u8 gpix0 = 0xff;
			uae_u32 dpix_val0 = BLANK_COLOR;
			uae_u8 pix1 = 0;
			uae_u8 gpix1 = 0xff;
			uae_u32 dpix_val1 = BLANK_COLOR;
			uae_u8 pix2 = 0;
			uae_u8 gpix2 = 0xff;
			uae_u32 dpix_val2 = BLANK_COLOR;
			uae_u8 pix3 = 0;
			uae_u8 gpix3 = 0xff;
			uae_u32 dpix_val3 = BLANK_COLOR;
			bool shifted = false;
			checkhorizontal1_ecs(denise_hcounter, denise_hcounter_next, h);
			if (!denise_blank_active) {
				dpix_val0 = bordercolor_ecs_shres;
				dpix_val1 = bordercolor_ecs_shres;
				dpix_val2 = bordercolor_ecs_shres;
				dpix_val3 = bordercolor_ecs_shres;
				if (denise_hdiw && bpl1dat_trigger) {
					shifted = true;
					pix0 = getbpl2();
					shiftbpl2();
					pix1 = getbpl2();
					shiftbpl2();
					pix2 = getbpl2();
					shiftbpl2();
					pix3 = getbpl2();
					shiftbpl2();
					get_shres_pix_genlock(pix0, pix1, &dpix_val0, &dpix_val1, &gpix0, &gpix1);
					get_shres_pix_genlock(pix2, pix3, &dpix_val2, &dpix_val3, &gpix2, &gpix3);
				}
			}
			uae_u32 sv0 = 0;
			if (denise_spr_nr_armeds) {
				uae_u32 svt;
				svt = denise_render_sprites_ecs_shres();
				if (!denise_sprite_blank_active && !sprites_hidden) {
					sv0 = svt;
				}
			}
			matchsprites_ecs_shres(cnt + 0);
			uae_u32 sv1 = 0;
			if (denise_spr_nr_armeds) {
				uae_u32 svt;
				svt = denise_render_sprites_ecs_shres();
				if (!denise_sprite_blank_active && !sprites_hidden) {
					sv1 = svt;
				}
			}
			matchsprites_ecs_shres(cnt + 2);
			if (sv0 || sv1) {
				get_shres_spr_pix(sv0, sv1, &dpix_val0, &dpix_val2);
				if (sv0) {
					dpix_val1 = dpix_val0;
				}
				if (sv1) {
					dpix_val3 = dpix_val2;
				}
			}
			if (denise_pixtotal >= 0 && denise_pixtotal < denise_pixtotal_max) {
				#ifdef DEBUGGER
				if (decode_specials_debug) {
					dpix_val0 = decode_denise_specials_debug(dpix_val0, cnt + 0);
				}
				#endif
				*buf1++ = dpix_val0;
				*buf2++ = dpix_val0;
				*gbuf++ = gpix0;
				#ifdef DEBUGGER
				if (decode_specials_debug) {
					dpix_val2 = decode_denise_specials_debug(dpix_val2, cnt + 2);
				}
				#endif
				*buf1++ = dpix_val2;
				*buf2++ = dpix_val2;
				*gbuf++ = gpix2;
			}
			if (!shifted) {
				shiftbpl2();
				shiftbpl2();
				shiftbpl2();
				shiftbpl2();
			}
			if (bpldat_copy[0] && (denise_hcounter_cmp & 3) == bplcon1_shift[0]) { 
				copybpl2();
			}
			internal_pixel_cnt += 4;
			sprites_hidden = sprites_hidden2;
			#ifdef DEBUGGER
			*debug_dma_dhpos_odd = denise_hcounter;
			#endif
			denise_hcounter_cmp++;
			denise_hcounter_cmp &= 511;
			denise_hcounter++;
			denise_hcounter &= 511;
			denise_hcounter_next++;
			denise_hcounter_next &= 511;
		}
		denise_pixtotal++;
		if (denise_pixtotal == 0) {
			internal_pixel_start_cnt = internal_pixel_cnt;
		}
		denise_hcounter = denise_hcounter_new;
		if (denise_accurate_mode) {
			denise_hcounter_cmp = denise_hcounter_new;
		}
		denise_cck++;
	}
}
static void lts_ecs_shres_dhires_genlock_filtered(void)
{
	while (denise_cck < denise_endcycle) {
		do_denise_cck(denise_linecnt, denise_startpos, denise_cck);
		if (lts_changed) return;
		for (int h = 0; h < 2; h++) {
			if (h) {
				denise_hcounter_next = denise_hcounter_new;
			}
			int cnt = denise_hcounter << 2;
			uae_u8 pix0 = 0;
			uae_u8 gpix0 = 0xff;
			uae_u32 dpix_val0 = BLANK_COLOR;
			uae_u8 pix1 = 0;
			uae_u8 gpix1 = 0xff;
			uae_u32 dpix_val1 = BLANK_COLOR;
			uae_u8 pix2 = 0;
			uae_u8 gpix2 = 0xff;
			uae_u32 dpix_val2 = BLANK_COLOR;
			uae_u8 pix3 = 0;
			uae_u8 gpix3 = 0xff;
			uae_u32 dpix_val3 = BLANK_COLOR;
			bool shifted = false;
			checkhorizontal1_ecs(denise_hcounter, denise_hcounter_next, h);
			if (!denise_blank_active) {
				dpix_val0 = bordercolor_ecs_shres;
				dpix_val1 = bordercolor_ecs_shres;
				dpix_val2 = bordercolor_ecs_shres;
				dpix_val3 = bordercolor_ecs_shres;
				if (denise_hdiw && bpl1dat_trigger) {
					shifted = true;
					pix0 = getbpl2();
					shiftbpl2();
					pix1 = getbpl2();
					shiftbpl2();
					pix2 = getbpl2();
					shiftbpl2();
					pix3 = getbpl2();
					shiftbpl2();
					get_shres_pix_genlock(pix0, pix1, &dpix_val0, &dpix_val1, &gpix0, &gpix1);
					get_shres_pix_genlock(pix2, pix3, &dpix_val2, &dpix_val3, &gpix2, &gpix3);
				}
			}
			uae_u32 sv0 = 0;
			if (denise_spr_nr_armeds) {
				uae_u32 svt;
				svt = denise_render_sprites_ecs_shres();
				if (!denise_sprite_blank_active && !sprites_hidden) {
					sv0 = svt;
				}
			}
			matchsprites_ecs_shres(cnt + 0);
			uae_u32 sv1 = 0;
			if (denise_spr_nr_armeds) {
				uae_u32 svt;
				svt = denise_render_sprites_ecs_shres();
				if (!denise_sprite_blank_active && !sprites_hidden) {
					sv1 = svt;
				}
			}
			matchsprites_ecs_shres(cnt + 2);
			if (sv0 || sv1) {
				get_shres_spr_pix(sv0, sv1, &dpix_val0, &dpix_val2);
				if (sv0) {
					dpix_val1 = dpix_val0;
				}
				if (sv1) {
					dpix_val3 = dpix_val2;
				}
			}
			if (denise_pixtotal >= 0 && denise_pixtotal < denise_pixtotal_max) {
				dpix_val0 = filter_pixel(dpix_val0, dpix_val1);
				dpix_val2 = filter_pixel(dpix_val2, dpix_val3);
				gpix0 = filter_pixel_genlock(gpix0, gpix1);
				gpix2 = filter_pixel_genlock(gpix2, gpix3);
				#ifdef DEBUGGER
				if (decode_specials_debug) {
					dpix_val0 = decode_denise_specials_debug(dpix_val0, cnt + 0);
				}
				#endif
				*buf1++ = dpix_val0;
				*buf2++ = dpix_val0;
				*gbuf++ = gpix0;
				#ifdef DEBUGGER
				if (decode_specials_debug) {
					dpix_val2 = decode_denise_specials_debug(dpix_val2, cnt + 2);
				}
				#endif
				*buf1++ = dpix_val2;
				*buf2++ = dpix_val2;
				*gbuf++ = gpix2;
			}
			if (!shifted) {
				shiftbpl2();
				shiftbpl2();
				shiftbpl2();
				shiftbpl2();
			}
			if (bpldat_copy[0] && (denise_hcounter_cmp & 3) == bplcon1_shift[0]) { 
				copybpl2();
			}
			internal_pixel_cnt += 4;
			sprites_hidden = sprites_hidden2;
			#ifdef DEBUGGER
			*debug_dma_dhpos_odd = denise_hcounter;
			#endif
			denise_hcounter_cmp++;
			denise_hcounter_cmp &= 511;
			denise_hcounter++;
			denise_hcounter &= 511;
			denise_hcounter_next++;
			denise_hcounter_next &= 511;
		}
		denise_pixtotal++;
		if (denise_pixtotal == 0) {
			internal_pixel_start_cnt = internal_pixel_cnt;
		}
		denise_hcounter = denise_hcounter_new;
		if (denise_accurate_mode) {
			denise_hcounter_cmp = denise_hcounter_new;
		}
		denise_cck++;
	}
}
static void lts_ecs_shres_dshres_genlock(void)
{
	while (denise_cck < denise_endcycle) {
		do_denise_cck(denise_linecnt, denise_startpos, denise_cck);
		if (lts_changed) return;
		for (int h = 0; h < 2; h++) {
			if (h) {
				denise_hcounter_next = denise_hcounter_new;
			}
			int cnt = denise_hcounter << 2;
			uae_u8 pix0 = 0;
			uae_u8 gpix0 = 0xff;
			uae_u32 dpix_val0 = BLANK_COLOR;
			uae_u8 pix1 = 0;
			uae_u8 gpix1 = 0xff;
			uae_u32 dpix_val1 = BLANK_COLOR;
			uae_u8 pix2 = 0;
			uae_u8 gpix2 = 0xff;
			uae_u32 dpix_val2 = BLANK_COLOR;
			uae_u8 pix3 = 0;
			uae_u8 gpix3 = 0xff;
			uae_u32 dpix_val3 = BLANK_COLOR;
			bool shifted = false;
			checkhorizontal1_ecs(denise_hcounter, denise_hcounter_next, h);
			if (!denise_blank_active) {
				dpix_val0 = bordercolor_ecs_shres;
				dpix_val1 = bordercolor_ecs_shres;
				dpix_val2 = bordercolor_ecs_shres;
				dpix_val3 = bordercolor_ecs_shres;
				if (denise_hdiw && bpl1dat_trigger) {
					shifted = true;
					pix0 = getbpl2();
					shiftbpl2();
					pix1 = getbpl2();
					shiftbpl2();
					pix2 = getbpl2();
					shiftbpl2();
					pix3 = getbpl2();
					shiftbpl2();
					get_shres_pix_genlock(pix0, pix1, &dpix_val0, &dpix_val1, &gpix0, &gpix1);
					get_shres_pix_genlock(pix2, pix3, &dpix_val2, &dpix_val3, &gpix2, &gpix3);
				}
			}
			uae_u32 sv0 = 0;
			if (denise_spr_nr_armeds) {
				uae_u32 svt;
				svt = denise_render_sprites_ecs_shres();
				if (!denise_sprite_blank_active && !sprites_hidden) {
					sv0 = svt;
				}
			}
			matchsprites_ecs_shres(cnt + 0);
			uae_u32 sv1 = 0;
			if (denise_spr_nr_armeds) {
				uae_u32 svt;
				svt = denise_render_sprites_ecs_shres();
				if (!denise_sprite_blank_active && !sprites_hidden) {
					sv1 = svt;
				}
			}
			matchsprites_ecs_shres(cnt + 2);
			if (sv0 || sv1) {
				get_shres_spr_pix(sv0, sv1, &dpix_val0, &dpix_val2);
				if (sv0) {
					dpix_val1 = dpix_val0;
				}
				if (sv1) {
					dpix_val3 = dpix_val2;
				}
			}
			if (denise_pixtotal >= 0 && denise_pixtotal < denise_pixtotal_max) {
				#ifdef DEBUGGER
				if (decode_specials_debug) {
					dpix_val0 = decode_denise_specials_debug(dpix_val0, cnt + 0);
				}
				#endif
				*buf1++ = dpix_val0;
				*buf2++ = dpix_val0;
				*gbuf++ = gpix0;
				#ifdef DEBUGGER
				if (decode_specials_debug) {
					dpix_val1 = decode_denise_specials_debug(dpix_val1, cnt + 1);
				}
				#endif
				*buf1++ = dpix_val1;
				*buf2++ = dpix_val1;
				*gbuf++ = gpix1;
				#ifdef DEBUGGER
				if (decode_specials_debug) {
					dpix_val2 = decode_denise_specials_debug(dpix_val2, cnt + 2);
				}
				#endif
				*buf1++ = dpix_val2;
				*buf2++ = dpix_val2;
				*gbuf++ = gpix2;
				#ifdef DEBUGGER
				if (decode_specials_debug) {
					dpix_val3 = decode_denise_specials_debug(dpix_val3, cnt + 3);
				}
				#endif
				*buf1++ = dpix_val3;
				*buf2++ = dpix_val3;
				*gbuf++ = gpix3;
			}
			if (!shifted) {
				shiftbpl2();
				shiftbpl2();
				shiftbpl2();
				shiftbpl2();
			}
			if (bpldat_copy[0] && (denise_hcounter_cmp & 3) == bplcon1_shift[0]) { 
				copybpl2();
			}
			internal_pixel_cnt += 4;
			sprites_hidden = sprites_hidden2;
			#ifdef DEBUGGER
			*debug_dma_dhpos_odd = denise_hcounter;
			#endif
			denise_hcounter_cmp++;
			denise_hcounter_cmp &= 511;
			denise_hcounter++;
			denise_hcounter &= 511;
			denise_hcounter_next++;
			denise_hcounter_next &= 511;
		}
		denise_pixtotal++;
		if (denise_pixtotal == 0) {
			internal_pixel_start_cnt = internal_pixel_cnt;
		}
		denise_hcounter = denise_hcounter_new;
		if (denise_accurate_mode) {
			denise_hcounter_cmp = denise_hcounter_new;
		}
		denise_cck++;
	}
}
static LINETOSRC_FUNC linetoscr_ecs_shres_genlock_funcs[] = {
	lts_ecs_shres_dhires_genlock,
	lts_ecs_shres_dhires_genlock,
	lts_ecs_shres_dshres_genlock,
	NULL
};
static LINETOSRC_FUNC linetoscr_ecs_shres_genlock_funcs_filtered[] = {
	NULL
};
