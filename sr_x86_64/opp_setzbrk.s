#################################################################
#								#
# Copyright (c) 2013-2015 Fidelity National Information 	#
# Services, Inc. and/or its subsidiaries. All rights reserved.	#
#								#
#	This source code contains the intellectual property	#
#	of its copyright holder(s), and is made available	#
#	under a license.  If you do not know the terms of	#
#	the license, please stop and do not read further.	#
#								#
#################################################################

	.include "linkage.si"
	.include "g_msf.si"
	.include "debug.si"

	.data
	.extern	frame_pointer

	.text
	.extern	op_setzbrk

ENTRY	opp_setzbrk
	putframe
	addq	$8, REG_SP		# Burn return PC & 16 byte align stack
	CHKSTKALIGN			# Verify stack alignment
	call	op_setzbrk
	getframe
	ret
