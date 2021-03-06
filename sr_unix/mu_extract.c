/****************************************************************
 *								*
 * Copyright (c) 2001-2015 Fidelity National Information 	*
 * Services, Inc. and/or its subsidiaries. All rights reserved.	*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"

#include "gtm_string.h"

#include <errno.h>
#include "gtm_fcntl.h"
#include "gtm_unistd.h"
#include "gtm_iconv.h"
#include "gtm_stdio.h"
#include "gtm_stat.h"

#ifdef __MVS__
#include "gtm_zos_io.h"
#endif
#include "stp_parms.h"
#include "gdsroot.h"
#include "gdsblk.h"
#include "gtm_facility.h"
#include "fileinfo.h"
#include "gdsbt.h"
#include "gdsfhead.h"
#include "muextr.h"
#include "cli.h"
#include "io.h"
#include "iosp.h"
#include "gtmio.h"
#include "io_params.h"
#include "eintr_wrappers.h"
#include "error.h"
#include "util.h"
#include "gtm_caseconv.h"
#include "op.h"
#include "mupip_exit.h"
#include "is_raw_dev.h"
#include "gv_select.h"
#include "mu_outofband_setup.h"
#include "gtmmsg.h"
#include "mvalconv.h"
#include "gtm_conv.h"
#include "gtm_utf8.h"
#include "filestruct.h"		/* needed for jnl.h */
#include "hashtab_mname.h"
#include "gvcst_protos.h"	/* for gvcst_root_search in DO_OP_GVNAME macro */
#include "change_reg.h"		/* for change_reg call in DO_OP_GVNAME macro */
#include "mu_getlst.h"
#include "gdskill.h"		/* needed for tp.h */
#include "jnl.h"			/* needed for tp.h */
#include "gdscc.h"			/* needed for tp.h */
#include "buddy_list.h"		/* needed for tp.h */
#include "hashtab_int4.h"	/* needed for tp.h */
#include "tp.h"

GBLREF	int		(*op_open_ptr)(mval *v, mval *p, int t, mval *mspace);
GBLREF	bool		mu_ctrlc_occurred;
GBLREF	bool		mu_ctrly_occurred;
GBLREF	gd_region	*gv_cur_region;
GBLREF	gd_addr		*gd_header;
GBLREF	io_pair          io_curr_device;
GBLREF	io_desc          *active_device;
GBLREF	gv_namehead	*gv_target;
GBLREF	boolean_t	jnlpool_init_needed;
GBLREF	mstr		sys_output;
GBLREF	tp_region	*grlist;

error_def(ERR_EXTRACTCTRLY);
error_def(ERR_EXTRACTFILERR);
error_def(ERR_MUNOACTION);
error_def(ERR_MUNOFINISH);
error_def(ERR_MUPCLIERR);
error_def(ERR_NOSELECT);
error_def(ERR_NULLCOLLDIFF);
error_def(ERR_RECORDSTAT);
error_def(ERR_EXTRFILEXISTS);
error_def(ERR_DBNOREGION);

LITDEF mval	mu_bin_datefmt	= DEFINE_MVAL_LITERAL(MV_STR, 0, 0, SIZEOF(BIN_HEADER_DATEFMT) - 1,
						      BIN_HEADER_DATEFMT, 0, 0);

LITREF mstr		chset_names[];

static readonly unsigned char	datefmt_txt[] = "DD-MON-YEAR  24:60:SS";
static readonly unsigned char	select_text[] = "SELECT";
static readonly mval		datefmt = DEFINE_MVAL_LITERAL(MV_STR, 0, 0, SIZEOF(datefmt_txt) - 1, (char *)datefmt_txt, 0, 0);
static readonly mval		null_str = DEFINE_MVAL_LITERAL(MV_STR, 0, 0, 0, 0, 0, 0);
static char			outfilename[256];
static unsigned short		filename_len;
static unsigned char		ochset_set = FALSE;
static readonly unsigned char	open_params_list[] =
{
	(unsigned char)iop_noreadonly,
	(unsigned char)iop_m,
	(unsigned char)iop_stream,
	(unsigned char)iop_nowrap,
	(unsigned char)iop_eol
};
static readonly unsigned char	use_params[] =
{
	(unsigned char)iop_nowrap,
	(unsigned char)iop_eol
};

static readonly unsigned char no_param = (unsigned char)iop_eol;

#define BINARY_FORMAT_STRING	"BINARY"
#define ZWR_FORMAT_STRING	"ZWR"
#define GO_FORMAT_STRING	"GO"

#define	WRITE_NUMERIC(nmfield)						\
{									\
	MV_FORCE_MVAL(&val, nmfield);					\
	n2s(&val);							\
	assertpro(!(val.mvtype & MV_NUM_APPROX));			\
	assertpro(BIN_HEADER_NUMSZ >= val.str.len);			\
	for (iter = val.str.len;  iter < BIN_HEADER_NUMSZ;  iter++)	\
		*outptr++ = '0';					\
	memcpy(outptr, val.str.addr, val.str.len);			\
	outptr += val.str.len;						\
}

#define GET_BIN_HEADER_SIZE(LABEL) (SIZEOF(LABEL) + SIZEOF(BIN_HEADER_DATEFMT) - 1 + 4 * BIN_HEADER_NUMSZ + BIN_HEADER_LABELSZ)
CONDITION_HANDLER(mu_extract_handler)
{
	mval				op_val, op_pars;
	unsigned char			delete_params[2] = { (unsigned char)iop_delete, (unsigned char)iop_eol };

	START_CH(TRUE);
	op_val.mvtype = op_pars.mvtype = MV_STR;
	op_val.str.addr = (char *)outfilename;
	op_val.str.len = filename_len;
	op_pars.str.len = SIZEOF(delete_params);
	op_pars.str.addr = (char *)delete_params;
	op_close(&op_val, &op_pars);

	util_out_print("!/MUPIP is not able to create extract file !AD due to the above error!/",
			TRUE, filename_len, outfilename);
	NEXTCH;
}

CONDITION_HANDLER(mu_extract_handler1)
{
	START_CH(TRUE);
	util_out_print("!/MUPIP is not able to complete the extract due the the above error!/", TRUE);
	util_out_print("!/WARNING!!!!!! Extract file !AD is incomplete!!!/",
			TRUE, filename_len, outfilename);
	NEXTCH;
}

void mu_extract(void)
{
	int 				stat_res, truncate_res;
	int				reg_max_rec, reg_max_key, reg_max_blk, reg_std_null_coll;
	int				iter, format, local_errno, int_nlen;
	boolean_t			freeze = FALSE, logqualifier, success, success2;
	char				format_buffer[FORMAT_STR_MAX_SIZE],  ch_set_name[MAX_CHSET_NAME], cli_buff[MAX_LINE],
					label_buff[LABEL_STR_MAX_SIZE];
	glist				gl_head, *gl_ptr, *next_gl_ptr;
	gd_region			*reg, *region_top;
	mu_extr_stats			global_total, grand_total, spangbl_total;
	uint4				item_code, devbufsiz, maxfield;
	unsigned short			label_len, n_len, ch_set_len, buflen;
	unsigned char			*outbuf, *outptr, *chptr, *leadptr;
	struct stat                     statbuf;
	mval				val, curr_gbl_name, op_val, op_pars;
	mstr				chset_mstr;
	mname_entry			gvname;
	gtm_chset_t 			saved_out_set;
	coll_hdr			extr_collhdr;
	int				bin_header_size;
	boolean_t			is_any_file_encrypted = FALSE;
	gvnh_reg_t			*gvnh_reg;
	gvnh_spanreg_t			*gvspan, *last_gvspan;
	boolean_t 			region;
#	ifdef GTM_CRYPT
	unsigned short			encrypted_hash_array_len;
	unsigned char			*curr_hash_ptr, *encrypted_hash_array_ptr;
	sgmnt_data_ptr_t		csd;
	sgmnt_addrs			*csa;
#	endif

	/* Initialize all local character arrays to zero before using */
	memset(cli_buff, 0, SIZEOF(cli_buff));
	memset(outfilename, 0, SIZEOF(outfilename));
	memset(label_buff, 0, SIZEOF(label_buff));
	memset(format_buffer, 0, SIZEOF(format_buffer));
	active_device = io_curr_device.out;
	mu_outofband_setup();
	if (CLI_PRESENT == cli_present("OCHSET"))
	{
		ch_set_len = SIZEOF(ch_set_name);
		if (cli_get_str("OCHSET", ch_set_name, &ch_set_len))
		{
			if (0 == ch_set_len)
				mupip_exit(ERR_MUNOACTION);	/* need to change to OPCHSET error when added */
			ch_set_name[ch_set_len] = '\0';
#			ifdef KEEP_zOS_EBCDIC
   			if ( (iconv_t)0 != active_device->output_conv_cd)
   			        ICONV_CLOSE_CD(active_device->output_conv_cd);
   			if (DEFAULT_CODE_SET != active_device->out_code_set)
   				ICONV_OPEN_CD(active_device->output_conv_cd, INSIDE_CH_SET, ch_set_name);
#			else
			chset_mstr.addr = ch_set_name;
			chset_mstr.len = ch_set_len;
			SET_ENCODING(active_device->ochset, &chset_mstr);
			get_chset_desc(&chset_names[active_device->ochset]);
#			endif
			ochset_set = TRUE;
		}
	}
	region = FALSE;
	if (CLI_PRESENT == cli_present("REGION"))
	{
		gvinit(); /*side effect: initializes gv_altkey (used by code below) & gv_currkey (not used by below code)*/
		mu_getlst("REGION", SIZEOF(tp_region));
		if (!grlist)
		{
			gtm_putmsg_csa(CSA_ARG(NULL) VARLSTCNT(1) ERR_DBNOREGION);
			mupip_exit(ERR_MUNOACTION);
		}
		region = TRUE;
	}
	logqualifier = (CLI_NEGATED != cli_present("LOG"));
	if (CLI_PRESENT == cli_present("FREEZE"))
		freeze = TRUE;
	n_len = SIZEOF(format_buffer);
	if (FALSE == cli_get_str("FORMAT", format_buffer, &n_len))
	{
		n_len = SIZEOF(ZWR_FORMAT_STRING) - 1;
		memcpy(format_buffer, ZWR_FORMAT_STRING, n_len);
	}
	int_nlen = n_len;
	lower_to_upper((uchar_ptr_t)format_buffer, (uchar_ptr_t)format_buffer, int_nlen);
	if (0 == memcmp(format_buffer, ZWR_FORMAT_STRING, n_len))
	        format = MU_FMT_ZWR;
	else if (0 == memcmp(format_buffer, GO_FORMAT_STRING, n_len))
	{
		if (gtm_utf8_mode)
		{
			util_out_print("Extract error: GO format is not supported in UTF-8 mode. Use ZWR format.", TRUE);
			mupip_exit(ERR_MUPCLIERR);
		}
		format = MU_FMT_GO;
	} else if (0 == memcmp(format_buffer, BINARY_FORMAT_STRING, n_len))
		format = MU_FMT_BINARY;
	else
	{
		util_out_print("Extract error: bad format type", TRUE);
		mupip_exit(ERR_MUPCLIERR);
	}
	n_len = SIZEOF(cli_buff);
	if (FALSE == cli_get_str((char *)select_text, cli_buff, &n_len))
	{
		n_len = 1;
		cli_buff[0] = '*';
	}
	/* gv_select will select globals */
	jnlpool_init_needed = TRUE;
        gv_select(cli_buff, n_len, freeze, (char *)select_text, &gl_head, &reg_max_rec, &reg_max_key, &reg_max_blk, region);
 	if (!gl_head.next)
        {
                rts_error_csa(CSA_ARG(NULL) VARLSTCNT(1) ERR_NOSELECT);
                mupip_exit(ERR_NOSELECT);
        }
	if (!region)
	{
		for (reg = gd_header->regions, region_top = gd_header->regions + gd_header->n_regions; reg < region_top; reg++)
		{
			if (reg->open)
				insert_region(reg, &(grlist), NULL, SIZEOF(tp_region));
		}
	}
	/* For binary format, check whether all regions have same null collation order */
	if (MU_FMT_BINARY == format)
	{
#		ifdef GTM_CRYPT
		encrypted_hash_array_len = GTMCRYPT_HASH_LEN * gd_header->n_regions;
		encrypted_hash_array_ptr = malloc(encrypted_hash_array_len);
		memset(encrypted_hash_array_ptr, 0, encrypted_hash_array_len);
#		endif
		tp_region *rptr;
		for (rptr = grlist, reg_std_null_coll = -1; NULL != rptr; rptr = rptr->fPtr)
		{
			reg = rptr->reg;
			if (reg->open)
			{
				if (reg_std_null_coll != reg->std_null_coll)
				{
					if (reg_std_null_coll == -1)
						reg_std_null_coll = reg->std_null_coll;
					else
					{
						rts_error_csa(CSA_ARG(REG2CSA(reg)) VARLSTCNT(1) ERR_NULLCOLLDIFF);
						mupip_exit(ERR_NULLCOLLDIFF);
					}
				}
#				ifdef GTM_CRYPT
				csa = &FILE_INFO(reg)->s_addrs;
				csd = csa->hdr;
				if (csd->is_encrypted)
				{
					curr_hash_ptr = encrypted_hash_array_ptr + (GTMCRYPT_HASH_LEN * find_reg_hash_idx(reg));
					memcpy(curr_hash_ptr, csd->encryption_hash, GTMCRYPT_HASH_LEN);
					is_any_file_encrypted = TRUE;
				}
#				endif
			}
		}
		assert(-1 != reg_std_null_coll);
	}
	MU_EXTR_STATS_INIT(grand_total);
	MU_EXTR_STATS_INIT(global_total);
	n_len = SIZEOF(outfilename);
	if (CLI_PRESENT == cli_present("STDOUT"))
		op_val.str = sys_output;	/* Redirect to standard output */
	else if (FALSE == cli_get_str("FILE", outfilename, &n_len))
	{
		rts_error_csa(CSA_ARG(NULL) VARLSTCNT(1) ERR_MUPCLIERR);
		mupip_exit(ERR_MUPCLIERR);
	} else if (-1 == Stat((char *)outfilename, &statbuf))
	{	/* Redirect to file */
		if (ENOENT != errno)
		{
			local_errno = errno;
			gtm_putmsg_csa(CSA_ARG(NULL) VARLSTCNT(5) ERR_EXTRACTFILERR, 2, LEN_AND_STR(outfilename), local_errno);
			mupip_exit(local_errno);
		}
		op_val.str.len = filename_len = n_len;
		op_val.str.addr = (char *)outfilename;
	} else
	{
		gtm_putmsg_csa(CSA_ARG(NULL) VARLSTCNT(4) ERR_EXTRFILEXISTS, 2, LEN_AND_STR(outfilename));
		mupip_exit(ERR_MUNOACTION);
	}
	op_pars.mvtype = MV_STR;
	op_pars.str.len = SIZEOF(open_params_list);
	op_pars.str.addr = (char *)open_params_list;
	op_val.mvtype = MV_STR;
	(*op_open_ptr)(&op_val, &op_pars, 0, 0);
	ESTABLISH(mu_extract_handler);
	op_pars.str.len = SIZEOF(use_params);
	op_pars.str.addr = (char *)&use_params;
	op_use(&op_val, &op_pars);
	if (MU_FMT_BINARY == format)
	{	/* binary header label format:
		 * fixed length text, fixed length date & time,
		 * fixed length max blk size, fixed length max rec size, fixed length max key size, fixed length std_null_coll
		 * 32-byte padded user-supplied string
		 */
		outbuf = (unsigned char *)malloc(SIZEOF(BIN_HEADER_LABEL) + SIZEOF(BIN_HEADER_DATEFMT) - 1 +
				4 * BIN_HEADER_NUMSZ + BIN_HEADER_LABELSZ);
		outptr = outbuf;
		if (is_any_file_encrypted)
		{
			MEMCPY_LIT(outptr, BIN_HEADER_LABEL_ENCR_INDEX);
			outptr += STR_LIT_LEN(BIN_HEADER_LABEL_ENCR_INDEX);
		} else
		{
			MEMCPY_LIT(outptr, BIN_HEADER_LABEL);
			outptr += STR_LIT_LEN(BIN_HEADER_LABEL);
		}
		op_horolog(&val);
		op_fnzdate(&val, (mval *)&mu_bin_datefmt, &null_str, &null_str, &val);
		memcpy(outptr, val.str.addr, val.str.len);
		outptr += val.str.len;
		WRITE_NUMERIC(reg_max_blk);
		WRITE_NUMERIC(reg_max_rec);
		WRITE_NUMERIC(reg_max_key);
		WRITE_NUMERIC(reg_std_null_coll);
		if (gtm_utf8_mode)
		{
			MEMCPY_LIT(outptr, UTF8_NAME);
			label_len = STR_LIT_LEN(UTF8_NAME);
			outptr[label_len++] = ' ';
		} else
			label_len = 0;
		buflen = SIZEOF(label_buff);
		if (FALSE == cli_get_str("LABEL", label_buff, &buflen))
		{
			MEMCPY_LIT(&outptr[label_len], EXTR_DEFAULT_LABEL);
			buflen = STR_LIT_LEN(EXTR_DEFAULT_LABEL);
		} else
			memcpy(&outptr[label_len], label_buff, buflen);
		label_len += buflen;
		if (label_len > BIN_HEADER_LABELSZ)
		{	/* Label size exceeds the space, so truncate the label and back off to the valid beginning
			 * (i.e. to the leading byte) of the last character that can entirely fit in the space
			 */
			label_len = BIN_HEADER_LABELSZ;
			chptr = &outptr[BIN_HEADER_LABELSZ];
			UTF8_LEADING_BYTE(chptr, outptr, leadptr);
			assert(chptr - leadptr < 4);
			if (leadptr < chptr)
				label_len -= (chptr - leadptr);
		}
		outptr += label_len;
		for (iter = label_len;  iter < BIN_HEADER_LABELSZ;  iter++)
			*outptr++ = ' ';
		label_len = outptr - outbuf;
		if (!ochset_set)
		{
#			ifdef KEEP_zOS_EBCDIC
			/* extract ascii header for binary by default */
			/* Do we need to restore it somewhere? */
			saved_out_set = (io_curr_device.out)->out_code_set;
			(io_curr_device.out)->out_code_set = DEFAULT_CODE_SET;
#			else
			saved_out_set = (io_curr_device.out)->ochset;
			(io_curr_device.out)->ochset = CHSET_M;
#			endif
		}
		op_val.str.addr = (char *)(&label_len);
		op_val.str.len = SIZEOF(label_len);
		op_write(&op_val);
		op_val.str.addr = (char *)outbuf;
		op_val.str.len = label_len;
		op_write(&op_val);
#		ifdef GTM_CRYPT
		if (is_any_file_encrypted)
		{
			op_val.str.addr = (char *)(&encrypted_hash_array_len);
			op_val.str.len = SIZEOF(encrypted_hash_array_len);
			op_write(&op_val);
			op_val.str.addr = (char *)encrypted_hash_array_ptr;
			op_val.str.len = encrypted_hash_array_len;
			op_write(&op_val);
		}
		assert(NULL != encrypted_hash_array_ptr);
		free(encrypted_hash_array_ptr);
#		endif
	} else
	{
		assert((MU_FMT_GO == format) || (MU_FMT_ZWR == format));
		label_len = SIZEOF(label_buff);
		if (FALSE == cli_get_str("LABEL", label_buff, &label_len))
		{
			MEMCPY_LIT(label_buff, EXTR_DEFAULT_LABEL);
			label_len = STR_LIT_LEN(EXTR_DEFAULT_LABEL);
		}
		if (gtm_utf8_mode)
		{
			label_buff[label_len++] = ' ';
			MEMCPY_LIT(&label_buff[label_len], UTF8_NAME);
			label_len += STR_LIT_LEN(UTF8_NAME);
		}
		label_buff[label_len++] = '\n';
		op_val.mvtype = MV_STR;
		op_val.str.len = label_len;
		op_val.str.addr = label_buff;
		op_write(&op_val);
		op_horolog(&val);
		op_fnzdate(&val, &datefmt, &null_str, &null_str, &val);
		op_val = val;
		op_val.mvtype = MV_STR;
		op_write(&op_val);
		if (MU_FMT_ZWR == format)
		{
			op_val.str.addr = " ZWR";
			op_val.str.len = SIZEOF(" ZWR") - 1;
			op_write(&op_val);
		}
		op_wteol(1);
	}
	REVERT;
	ESTABLISH(mu_extract_handler1);
	success = TRUE;
	gvspan = NULL;
	for (gl_ptr = gl_head.next; gl_ptr; gl_ptr = next_gl_ptr)
	{
		if (mu_ctrly_occurred)
			break;
		DO_OP_GVNAME(gl_ptr);
			/* sets gv_target/gv_currkey/gv_cur_region/cs_addrs/cs_data to correspond to <globalname,reg> in gl_ptr */
		if (MU_FMT_BINARY == format)
		{
			label_len = SIZEOF(extr_collhdr);
			op_val.mvtype = MV_STR;
			op_val.str.addr = (char *)(&label_len);
			op_val.str.len = SIZEOF(label_len);
			op_write(&op_val);
			extr_collhdr.act = gv_target->act;
			extr_collhdr.nct = gv_target->nct;
			extr_collhdr.ver = gv_target->ver;
			op_val.str.addr = (char *)(&extr_collhdr);
			op_val.str.len = SIZEOF(extr_collhdr);
			op_write(&op_val);
		}
#		ifdef GTM_CRYPT
		success2 = mu_extr_gblout(gl_ptr, &global_total, format, is_any_file_encrypted);
#		else
		success2 = mu_extr_gblout(gl_ptr, &global_total, format);
#		endif
		success = success2 && success;
		gvnh_reg = gl_ptr->gvnh_reg;
		last_gvspan = gvspan;
		gvspan = gvnh_reg->gvspan;
		if (NULL != gvspan)
		{	/* this global spans more than one region. aggregate stats across all regions */
			if (last_gvspan != gvspan)
				MU_EXTR_STATS_INIT(spangbl_total); /* this is the FIRST spanned region. initialize spangbl_total */
			MU_EXTR_STATS_ADD(spangbl_total, global_total);	/* add global_total to grand_total */
		}
		next_gl_ptr = gl_ptr->next;
		if ((logqualifier || mu_ctrlc_occurred) && (0 < global_total.recknt))
		{
			ISSUE_RECORDSTAT_MSG(gl_ptr, global_total, PRINT_REG_TRUE);
			if ((NULL != gvspan) && ((NULL == next_gl_ptr) || (next_gl_ptr->gvnh_reg != gvnh_reg)))
			{	/* this is the LAST spanned region. Display summary line across all spanned regions */
				ISSUE_RECORDSTAT_MSG(gl_ptr, spangbl_total, PRINT_REG_FALSE);
			}
			mu_ctrlc_occurred = FALSE;
		}
		MU_EXTR_STATS_ADD(grand_total, global_total);	/* add global_total to grand_total */
	}
	assert((MV_STR == op_val.mvtype) && (MV_STR == op_pars.mvtype));
	op_val.str.addr = (char *)outfilename;
	op_val.str.len = filename_len;
	op_pars.str.len = SIZEOF(no_param);
	op_pars.str.addr = (char *)&no_param;
	op_close(&op_val, &op_pars);
	REVERT;
	if (mu_ctrly_occurred)
	{
		gtm_putmsg_csa(CSA_ARG(NULL) VARLSTCNT(1) ERR_EXTRACTCTRLY);
		mupip_exit(ERR_MUNOFINISH);
	}
	if (0 != grand_total.recknt)
	{
		gtm_putmsg_csa(CSA_ARG(NULL) VARLSTCNT(8) ERR_RECORDSTAT, 6, LEN_AND_LIT("TOTAL"),
				&grand_total.recknt, grand_total.keylen, grand_total.datalen, grand_total.reclen);
	} else
	{
                gtm_putmsg_csa(CSA_ARG(NULL) VARLSTCNT(1) ERR_NOSELECT);
		UNLINK(outfilename);
                mupip_exit(ERR_NOSELECT);
	}
	mupip_exit(success ? SS_NORMAL : ERR_MUNOFINISH);
}
