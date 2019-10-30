/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the Apache License, Version 2.0  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file m2ap_mce_procedures.c
  \brief
  \author Dincer Beken
  \company Blackned GmbH
  \email: dbeken@blackned.de
*/


#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "bstrlib.h"

#include "dynamic_memory_check.h"
#include "assertions.h"
#include "hashtable.h"
#include "log.h"
#include "msc.h"
#include "conversions.h"
#include "intertask_interface.h"
#include "asn1_conversions.h"
#include "timer.h"

#include "mme_config.h"
#include "m2ap_common.h"
#include "m2ap_mce_encoder.h"
#include "m2ap_mce.h"
#include "m2ap_mce_itti_messaging.h"
#include "m2ap_mce_procedures.h"


/* Every time a new MBMS service is associated, increment this variable.
   But care if it wraps to increment also the mme_ue_s1ap_id_has_wrapped
   variable. Limit: UINT32_MAX (in stdint.h).
*/
//static mme_ue_s1ap_id_t                 mme_ue_s1ap_id = 0;
//static bool                             mme_ue_s1ap_id_has_wrapped = false;

extern const char                      *m2ap_direction2String[];
//static bool
//s1ap_add_bearer_context_to_setup_list (S1AP_E_RABToBeSetupListHOReqIEs_t * const e_RABToBeSetupListHOReq_p,
//    S1AP_E_RABToBeSetupItemHOReq_t        * e_RABToBeSetupHO_p, bearer_contexts_to_be_created_t * bc_tbc);

//static bool
//s1ap_add_bearer_context_to_switch_list (S1AP_E_RABToBeSwitchedULListIEs_t * const e_RABToBeSwitchedListHOReq_p,
//    S1AP_E_RABToBeSwitchedULItem_t        * e_RABToBeSwitchedHO_p, bearer_context_t * bearer_ctxt_p);

//------------------------------------------------------------------------------
void
m3ap_handle_mbms_session_start_request (
  const itti_m3ap_mbms_session_start_req_t * const mbms_session_start_req_pP)
{
  /*
   * MBMS-GW triggers a new MBMS Service. No eNBs are specified but only the MBMS service area.
   * MCE_APP will not be eNB specific. eNB specific messages will be handled in the M2AP_APP.
   * That a single eNB fails to broadcast, is not of importance to the MCE_APP.
   * This message initiates for all eNBs in the given MBMS Service are a new MBMS Bearer Service.
   */
  uint8_t                                *buffer_p = NULL;
  uint32_t                                length = 0;
  mbms_description_t               		 *mbms_ref = NULL;
  m2ap_enb_description_t                 *target_enb_ref = NULL;

  OAILOG_FUNC_IN (LOG_M2AP);
  DevAssert (mbms_session_start_req_pP != NULL);

  /*
   * We need to check the MBMS Service via TMGI and MBMS Service Index.
   * Currently, only their combination must be unique and only 1 SA can be activated at a time.
   *
   * MCE MBMS M2AP ID is 24 bits long, so we cannot use MBMS Service Index.
   */
  mbms_ref = m2ap_is_mbms_tmgi_in_list(&mbms_session_start_req_pP->tmgi, mbms_session_start_req_pP->mbms_service_area_id); /**< Nothing eNB specific. */
  if (mbms_ref) {
    /**
     * Just remove this implicitly without notifying the eNB.
     * There should be complications with the response, eNB should be able to handle this.
     */
    OAILOG_ERROR (LOG_M2AP, " An MBMS Service Description with for TMGI " TMGI_FMT " and MBMS_Service_Area ID " MBMS_SERVICE_AREA_ID_FMT "already exists. Removing implicitly. \n",
        TMGI_ARG(&mbms_session_start_req_pP->tmgi), mbms_session_start_req_pP->mbms_service_area_id);
    m2ap_remove_mbms(mbms_ref);
  }

  /** Check that there exists at least a single eNB with the MBMS Service Area (we don't start MBMS sessions for eNBs which later on connected). */
  mme_config_read_lock (&mme_config);
  m2ap_enb_description_t *			         m2ap_enb_p_elements[mme_config.max_m2_enbs];
  memset(&m2ap_enb_p_elements, 0, (sizeof(m2ap_enb_description_t*) * mme_config.max_m2_enbs));
  mme_config_unlock (&mme_config);
  int num_m2ap_enbs = 0;
  m2ap_is_mbms_sai_in_list(mbms_session_start_req_pP->mbms_service_area_id, &num_m2ap_enbs, (m2ap_enb_description_t **)&m2ap_enb_p_elements);
  if(!num_m2ap_enbs){
    OAILOG_ERROR (LOG_M2AP, "No M2AP eNBs could be found for the MBMS SA " MBMS_SERVICE_AREA_ID_FMT" for the MBMS Service with TMGI " TMGI_FMT". \n",
    	mbms_session_start_req_pP->mbms_service_area_id, TMGI_ARG(&mbms_session_start_req_pP->tmgi));
    OAILOG_FUNC_OUT (LOG_M2AP);
  }

  /**
   * We don't care to inform the MCE_APP layer.
   * Create a new MBMS Service Description.
   * Allocate an MCE M2AP MBMS ID (24) inside it. Will be used for all eNB associations.
   */
  if((mbms_ref = m2ap_new_mbms (&mbms_session_start_req_pP->tmgi, mbms_session_start_req_pP->mbms_service_area_id)) == NULL) {
    // If we failed to allocate a new MBMS Service Description return -1
    OAILOG_ERROR (LOG_M2AP, "M2AP:MBMS Session Start Request- Failed to allocate M2AP Service Description for TMGI " TMGI_FMT " and MBMS Service Area Id "MBMS_SERVICE_AREA_ID_FMT". \n",
    	TMGI_ARG(&mbms_session_start_req_pP->tmgi), mbms_session_start_req_pP->mbms_service_area_id);
    OAILOG_FUNC_OUT (LOG_M2AP);
  }

  /**
   * Update the created MBMS Service Description.
   * Directly set the values and don't wait for a response, we will set these values into the eNB, once the timer runs out.
   * We don't need the MBMS Session Duration. It will be handled in the MCE_APP layer.
   */
  memcpy((void*)&mbms_ref->mbms_bc.mbms_ip_mc_distribution,  (void*)&mbms_session_start_req_pP->mbms_bearer_tbc.mbms_ip_mc_dist, sizeof(mbms_ip_multicast_distribution_t));
  memcpy((void*)&mbms_ref->mbms_bc.eps_bearer_context.bearer_level_qos, (void*)&mbms_session_start_req_pP->mbms_bearer_tbc.bc_tbc.bearer_level_qos, sizeof(bearer_qos_t));

  // todo: no state! --> Check if Start & Update work without state!

  /**
   * Check if a timer has been given, if so start the timer.
   * If not send immediately.
   */
  if(mbms_session_start_req_pP->time_to_start_in_sec){
    OAILOG_INFO (LOG_M2AP, "M2AP:MBMS Session Start Request- Received a timer of (%d)s for start of TMGI " TMGI_FMT " and MBMS Service Area ID "MBMS_SERVICE_AREA_ID_FMT". \n",
    	mbms_session_start_req_pP->time_to_start_in_sec, TMGI_ARG(&mbms_session_start_req_pP->tmgi), mbms_session_start_req_pP->mbms_service_area_id);
    if (timer_setup (mbms_session_start_req_pP->time_to_start_in_sec, mbms_session_start_req_pP->time_to_start_in_usec,
           TASK_M2AP, INSTANCE_DEFAULT, TIMER_ONE_SHOT, (void*)mbms_ref->mce_mbms_m2ap_id, &(mbms_ref->m2ap_action_timer.id)) < 0) {
         OAILOG_ERROR (LOG_M2AP, "Failed to start MBMS Sesssion Start timer for MBMS with MBMS M2AP MCE ID " MCE_MBMS_M2AP_ID_FMT". \n", mbms_ref->mce_mbms_m2ap_id);
         mbms_ref->m2ap_action_timer.id = M2AP_TIMER_INACTIVE_ID;
         /** Send immediately. */
       } else {
         OAILOG_DEBUG (LOG_M2AP, "Started M2AP MBMS Session start timer (timer id 0x%lx) for MBMS Session MBMS M2AP MCE ID " MCE_MBMS_M2AP_ID_FMT". "
        		 "Waiting for timeout to trigger M2AP Session Start to M2AP eNBs.\n", mbms_ref->m2ap_action_timer.id, mbms_ref->mce_mbms_m2ap_id);
         /** Leave the method. */
         OAILOG_FUNC_OUT(LOG_M2AP);
       }
  }

  m2ap_generate_mbms_session_start_request(mbms_ref->mce_mbms_m2ap_id);
  OAILOG_FUNC_OUT(LOG_M2AP);
}

//------------------------------------------------------------------------------
int m2ap_generate_mbms_session_start_request(mce_mbms_m2ap_id_t mbms_m2ap_id)
{
  OAILOG_FUNC_IN (LOG_M2AP);
  mbms_description_t                     *mbms_ref = NULL;
  uint8_t                                *buffer_p = NULL;
  uint32_t                                length = 0;
  MessagesIds                             message_id = MESSAGES_ID_MAX;
  void                                   *id = NULL;
  M2AP_M2AP_PDU_t                         pdu = {0};
  M2AP_SessionStartRequest_t			 *out;
  M2AP_SessionStartRequest_Ies_t		 *ie = NULL;

  mbms_ref = m2ap_is_mbms_mce_m2ap_id_in_list(mbms_m2ap_id);
  if (!mbms_ref) {
	OAILOG_ERROR (LOG_M2AP, "No MBMS MCE M2AP ID " MCE_MBMS_M2AP_ID_FMT". Cannot generate MBMS Session Start Request. \n", mbms_m2ap_id);
    OAILOG_FUNC_RETURN (LOG_M2AP, RETURNerror);
  }
  /*
   * We have found the UE in the list.
   * Create new IE list message and encode it.
   */
  memset(&pdu, 0, sizeof(pdu));
  pdu.present = M2AP_M2AP_PDU_PR_initiatingMessage;
  pdu.choice.initiatingMessage.procedureCode = M2AP_ProcedureCode_id_sessionStart;
  pdu.choice.initiatingMessage.criticality = M2AP_Criticality_ignore;
  pdu.choice.initiatingMessage.value.present = M2AP_InitiatingMessage__value_PR_SessionStartRequest;
  out = &pdu.choice.initiatingMessage.value.choice.SessionStartRequest;

  /*
   * Setting MBMS informations with the ones found in ue_ref
   */
  /* mandatory */
  ie = (M2AP_SessionStartRequest_Ies_t *)calloc(1, sizeof(M2AP_SessionStartRequest_Ies_t));
  ie->id = M2AP_ProtocolIE_ID_id_MCE_MBMS_M2AP_ID;
  ie->criticality = M2AP_Criticality_reject;
  ie->value.present = M2AP_SessionStartRequest_Ies__value_PR_MCE_MBMS_M2AP_ID;
  ie->value.choice.MCE_MBMS_M2AP_ID = mbms_m2ap_id;
  ASN_SEQUENCE_ADD(&out->protocolIEs.list, ie);

  /* mandatory */
  ie = (M2AP_SessionStartRequest_Ies_t *)calloc(1, sizeof(M2AP_SessionStartRequest_Ies_t));
  ie->id = M2AP_ProtocolIE_ID_id_TMGI;
  ie->criticality = M2AP_Criticality_reject;
  ie->value.present = M2AP_SessionStartRequest_Ies__value_PR_TMGI;
//  ie->value.choice.TMGI.= mbms_m2ap_id;
  ASN_SEQUENCE_ADD(&out->protocolIEs.list, ie);
//
//    /*eNB
//     * Fill in the NAS pdu
//     */
//			//S1AP_UEAggregateMaximumBitrate_t	 UEAggregateMaximumBitrate;
//
////    if (e_rab_release_req->ue_aggregate_maximum_bit_rate_present) {
////      e_rabreleasecommandies->presenceMask |= S1AP_E_RABRELEASECOMMANDIES_UEAGGREGATEMAXIMUMBITRATE_PRESENT;
////      TO DO e_rabreleasecommandies->uEaggregateMaximumBitrate.uEaggregateMaximumBitRateDL.buf
////    }
//    ie = (S1AP_E_RABReleaseCommandIEs_t *)calloc(1, sizeof(S1AP_E_RABReleaseCommandIEs_t));
//    ie->id = S1AP_ProtocolIE_ID_id_E_RABToBeReleasedList;
//    ie->criticality = S1AP_Criticality_ignore;
//    ie->value.present = S1AP_E_RABReleaseCommandIEs__value_PR_E_RABList;
//
//    ASN_SEQUENCE_ADD (&out->protocolIEs.list, ie);
//    S1AP_E_RABList_t	* const e_rab_list= &ie->value.choice.E_RABList;
//
//    for  (int i= 0; i < e_rab_release_req->e_rab_to_be_release_list.no_of_items; i++) {
//      S1AP_E_RABItemIEs_t * s1ap_e_rab_item_ies = calloc(1, sizeof(S1AP_E_RABItemIEs_t));
//      s1ap_e_rab_item_ies->id = S1AP_ProtocolIE_ID_id_E_RABReleaseItem;
//      s1ap_e_rab_item_ies->criticality = S1AP_Criticality_ignore;
//      s1ap_e_rab_item_ies->value.present = S1AP_E_RABItemIEs__value_PR_E_RABItem;
//
//      S1AP_E_RABItem_t  *s1ap_e_rab_item = &s1ap_e_rab_item_ies->value.choice.E_RABItem;
//
//      s1ap_e_rab_item->e_RAB_ID = e_rab_release_req->e_rab_to_be_release_list.item[i].e_rab_id;
//      /** Set Id-Cause. */
//      s1ap_e_rab_item->cause.present = S1AP_Cause_PR_nas;
//      s1ap_e_rab_item->cause.choice.nas = 0;
//
//      ASN_SEQUENCE_ADD (&e_rab_list->list, s1ap_e_rab_item_ies);
//    }
//
//    /** Set the NAS message outside of the EBI list. */
//    if(e_rab_release_req->nas_pdu){
//      ie = (S1AP_E_RABReleaseCommandIEs_t *)calloc(1, sizeof(S1AP_E_RABReleaseCommandIEs_t));
//      ie->id = S1AP_ProtocolIE_ID_id_NAS_PDU;
//      ie->criticality = S1AP_Criticality_ignore;
//      ie->value.present = S1AP_E_RABReleaseCommandIEs__value_PR_NAS_PDU;
//
//  	  S1AP_NAS_PDU_t	 * nas_pdu = &ie->value.choice.NAS_PDU;
//      OCTET_STRING_fromBuf (nas_pdu, (char *)bdata(e_rab_release_req->nas_pdu), blength(e_rab_release_req->nas_pdu));
//      ASN_SEQUENCE_ADD (&out->protocolIEs.list, ie);
//    }else{
//      OAILOG_INFO(LOG_S1AP, "No NAS message received for S1AP E-RAB release command for ueId " MME_UE_S1AP_ID_FMT" .\n", e_rab_release_req->mme_ue_s1ap_id);
//    }

  if (m2ap_mme_encode_pdu (&pdu, &buffer_p, &length) < 0) {
	// TODO: handle something
	OAILOG_ERROR (LOG_M2AP, "Failed to encode MBMS Session Start Request. \n");
	OAILOG_FUNC_RETURN (LOG_S1AP, RETURNerror);
  }

  OAILOG_NOTICE (LOG_M2AP, "Send M2AP_MBMS_SESSION_START_REQUEST message MCE_MBMS_M2AP_ID = " MCE_MBMS_M2AP_ID_FMT "\n", mbms_m2ap_id);
  // todo: the next_sctp_stream is the one without incrementation?
  bstring b = blk2bstr(buffer_p, length);
  free(buffer_p);
//  m2ap_mce_itti_send_sctp_request(&b, target_enb_ref->sctp_assoc_id, target_enb_ref->next_sctp_stream, INVALID_MBMS_SERVICE_INDEX);
//  s1ap_mme_itti_send_sctp_request (&b , ue_ref->enb->sctp_assoc_id, ue_ref->sctp_stream_send, ue_ref->mme_ue_s1ap_id);
  OAILOG_FUNC_RETURN (LOG_S1AP, RETURNok);
}

//------------------------------------------------------------------------------
void
m3ap_handle_mbms_session_stop_request (
  const itti_m3ap_mbms_session_stop_req_t * const mbms_session_stop_req_pP)
{
  /*
   * MBMS-GW triggers the stop of an MBMS Service on all the eNBs which are receiving it.
   */
  uint8_t                                *buffer_p = NULL;
  uint32_t                                length = 0;
  mbms_description_t               		 *mbms_ref = NULL;
  m2ap_enb_description_t                 *target_enb_ref = NULL;
  M2AP_M2AP_PDU_t                         pdu = {0};
//  S1AP_HandoverRequest_t		   		 *out;
//  S1AP_HandoverRequestIEs_t  			 *ie = NULL;

  OAILOG_FUNC_IN (LOG_M2AP);
  DevAssert (mbms_session_stop_req_pP != NULL);

//  /*
//   * Based on the MCE_MBMS_M2AP_ID, you may or may not have a UE reference, we don't care. Just send HO_Request to the new eNB.
//   * Check that there exists an enb reference to the target-enb.
//   */
//  target_enb_ref = s1ap_is_enb_id_in_list(handover_req_pP->macro_enb_id);
//  if(!target_enb_ref){
//    OAILOG_ERROR (LOG_M2AP, "No target-enb could be found for enb-id %u. Handover Failed. \n",
//            handover_req_pP->macro_enb_id);
//    /**
//     * Send Handover Failure back (manually) to the MME.
//     * This will trigger an implicit detach if the UE is not REGISTERED yet (single MME S1AP HO), or will just send a HO-PREP-FAILURE to the MME (if the cause is not S1AP-SYSTEM-FAILURE).
//     */
//    MessageDef                             *message_p = NULL;
//    message_p = itti_alloc_new_message (TASK_S1AP, S1AP_HANDOVER_FAILURE);
//    AssertFatal (message_p != NULL, "itti_alloc_new_message Failed");
//    itti_s1ap_handover_failure_t *handover_failure_p = &message_p->ittiMsg.s1ap_handover_failure;
//    memset ((void *)&message_p->ittiMsg.s1ap_handover_failure, 0, sizeof (itti_s1ap_handover_failure_t));
//    /** Fill the S1AP Handover Failure elements per hand. */
//    handover_failure_p->mme_ue_s1ap_id = handover_req_pP->ue_id;
//    /** No need to remove any UE_Reference to the target_enb, not existing. */
//    itti_send_msg_to_task (TASK_MME_APP, INSTANCE_DEFAULT, message_p);
//    OAILOG_FUNC_OUT (LOG_M2AP);
//  }
//
//  /**
//   * UE Reference will only be created with a valid ENB_UE_S1AP_ID!
//   * We don't wan't to save 2 the UE reference twice in the hashmap, but only with a valid ENB_ID key.
//   * That's why create the UE Reference only with Handover Request Acknowledge.
//   * No timer will be created for Handover Request (not defined in the specification and no UE-Reference to the target-ENB exists yet.
//   *
//   * Target eNB could be found. Create a new ue_reference.
//   * This UE eNB Id has currently no known s1 association.
//   * * * * Create new UE context by associating new mme_ue_s1ap_id.
//   * * * * Update eNB UE list.
//   *
//   * todo: what to provide as enb_id?
//   */
//
//  /*
//   * Start the outcome response timer.
//   *
//   * * * * When time is reached, MME consider that procedure outcome has failed.
//   */
//  //     timer_setup(mme_config.s1ap_config.outcome_drop_timer_sec, 0, TASK_S1AP, INSTANCE_DEFAULT,
//  //                 TIMER_ONE_SHOT,
//  //                 NULL,
//  //                 &ue_ref->outcome_response_timer_id);
//  /*
//   * Insert the timer in the MAP of mme_ue_s1ap_id <-> timer_id
//   */
//  //     s1ap_timer_insert(ue_ref->mme_ue_s1ap_id, ue_ref->outcome_response_timer_id);
//  // todo: PSR if the state is handover, else just complete the message!
//  memset(&pdu, 0, sizeof(pdu));
//  pdu.present = M2AP_M2AP_PDU_PR_initiatingMessage;
//  pdu.choice.initiatingMessage.procedureCode = S1AP_ProcedureCode_id_HandoverResourceAllocation;
//  pdu.choice.initiatingMessage.criticality = M2AP_Criticality_ignore;
//  pdu.choice.initiatingMessage.value.present = S1AP_InitiatingMessage__value_PR_HandoverRequest;
//  out = &pdu.choice.initiatingMessage.value.choice.HandoverRequest;
//
//  /* mandatory */
//  ie = (S1AP_HandoverRequestIEs_t *)calloc(1, sizeof(S1AP_HandoverRequestIEs_t));
//  ie->id = S1AP_ProtocolIE_ID_id_MCE_MBMS_M2AP_ID;
//  ie->criticality = M2AP_Criticality_reject;
//  ie->value.present = S1AP_HandoverRequestIEs__value_PR_MCE_MBMS_M2AP_ID;
//  ie->value.choice.MCE_MBMS_M2AP_ID = handover_req_pP->ue_id;
//  ASN_SEQUENCE_ADD(&out->protocolIEs.list, ie);
//
//  /** Set Handover Type. */
//  ie = (S1AP_HandoverRequestIEs_t *)calloc(1, sizeof(S1AP_HandoverRequestIEs_t));
//  ie->id = S1AP_ProtocolIE_ID_id_HandoverType;
//  ie->criticality = M2AP_Criticality_reject;
//  ie->value.present = S1AP_HandoverRequestIEs__value_PR_HandoverType;
//  ie->value.choice.HandoverType = S1AP_HandoverType_intralte;
//  ASN_SEQUENCE_ADD(&out->protocolIEs.list, ie);
//
//  /** Set Id-Cause. */
//  ie = (S1AP_HandoverRequestIEs_t *)calloc(1, sizeof(S1AP_HandoverRequestIEs_t));
//  ie->id = S1AP_ProtocolIE_ID_id_HO_Cause;
//  ie->criticality = M2AP_Criticality_reject;
//  ie->value.present = S1AP_HandoverRequestIEs__value_PR_Cause;
//  ie->value.choice.Cause.present = S1AP_Cause_PR_radioNetwork;
//  ie->value.choice.Cause.choice.radioNetwork = S1AP_CauseRadioNetwork_handover_desirable_for_radio_reason;
//  ASN_SEQUENCE_ADD(&out->protocolIEs.list, ie);
//
//  /*
//   * uEaggregateMaximumBitrateDL and uEaggregateMaximumBitrateUL expressed in term of bits/sec
//   */
//  ie = (S1AP_HandoverRequestIEs_t *)calloc(1, sizeof(S1AP_HandoverRequestIEs_t));
//  ie->id = S1AP_ProtocolIE_ID_id_uEaggregateMaximumBitrate;
//  ie->criticality = M2AP_Criticality_reject;
//  ie->value.present = S1AP_HandoverRequestIEs__value_PR_UEAggregateMaximumBitrate;
//  asn_uint642INTEGER (&ie->value.choice.UEAggregateMaximumBitrate.uEaggregateMaximumBitRateDL, handover_req_pP->ambr.br_dl);
//  asn_uint642INTEGER (&ie->value.choice.UEAggregateMaximumBitrate.uEaggregateMaximumBitRateUL, handover_req_pP->ambr.br_ul);
//  ASN_SEQUENCE_ADD(&out->protocolIEs.list, ie);
//
//  /** Set the UE security capabilities. */
//  ie = (S1AP_HandoverRequestIEs_t *)calloc(1, sizeof(S1AP_HandoverRequestIEs_t));
//  ie->id = S1AP_ProtocolIE_ID_id_UESecurityCapabilities;
//  ie->criticality = M2AP_Criticality_reject;
//  ie->value.present = S1AP_HandoverRequestIEs__value_PR_UESecurityCapabilities;
//  S1AP_UESecurityCapabilities_t	*const  ue_security_capabilities = &ie->value.choice.UESecurityCapabilities;
//  ue_security_capabilities->encryptionAlgorithms.buf = calloc(1, sizeof(uint16_t));
//  memcpy(ue_security_capabilities->encryptionAlgorithms.buf, &handover_req_pP->security_capabilities_encryption_algorithms, sizeof(uint16_t));
//  ue_security_capabilities->encryptionAlgorithms.size = 2;
//  ue_security_capabilities->encryptionAlgorithms.bits_unused = 0;
//  OAILOG_DEBUG (LOG_M2AP, "security_capabilities_encryption_algorithms 0x%04X\n", handover_req_pP->security_capabilities_encryption_algorithms);
//
//  ue_security_capabilities->integrityProtectionAlgorithms.buf = calloc(1, sizeof(uint16_t));
//  memcpy(ue_security_capabilities->integrityProtectionAlgorithms.buf, &handover_req_pP->security_capabilities_integrity_algorithms, sizeof(uint16_t));
//  ue_security_capabilities->integrityProtectionAlgorithms.size = 2;
//  ue_security_capabilities->integrityProtectionAlgorithms.bits_unused = 0;
//  OAILOG_DEBUG (LOG_M2AP, "security_capabilities_integrity_algorithms 0x%04X\n", handover_req_pP->security_capabilities_integrity_algorithms);
//  ASN_SEQUENCE_ADD(&out->protocolIEs.list, ie);
//
//  /** Add the security context. */
//  ie = (S1AP_HandoverRequestIEs_t *)calloc(1, sizeof(S1AP_HandoverRequestIEs_t));
//  ie->id = S1AP_ProtocolIE_ID_id_SecurityContext;
//  ie->criticality = M2AP_Criticality_reject;
//  ie->value.present = S1AP_HandoverRequestIEs__value_PR_SecurityContext;
//  if (handover_req_pP->nh) {
//	ie->value.choice.SecurityContext.nextHopParameter.buf = calloc (AUTH_NH_SIZE, sizeof(uint8_t));
//	memcpy (ie->value.choice.SecurityContext.nextHopParameter.buf, handover_req_pP->nh, AUTH_NH_SIZE);
//	ie->value.choice.SecurityContext.nextHopParameter.size = AUTH_NH_SIZE;
//  } else {
//	  OAILOG_DEBUG (LOG_M2AP, "No nh \n");
//	  ie->value.choice.SecurityContext.nextHopParameter.buf = NULL;
//	  ie->	value.choice.SecurityContext.nextHopParameter.size = 0;
//  }
//  ie->value.choice.SecurityContext.nextHopParameter.bits_unused = 0;
//  ie->value.choice.SecurityContext.nextHopChainingCount = handover_req_pP->ncc;
//  ASN_SEQUENCE_ADD(&out->protocolIEs.list, ie);
//
//  /*
//   * E-UTRAN Target-ToSource Transparent Container.
//   */
//  ie = (S1AP_HandoverRequestIEs_t *)calloc(1, sizeof(S1AP_HandoverRequestIEs_t));
//  ie->id = S1AP_ProtocolIE_ID_id_Source_ToTarget_TransparentContainer;
//  ie->criticality = M2AP_Criticality_reject;
//  ie->value.present = S1AP_HandoverRequestIEs__value_PR_Source_ToTarget_TransparentContainer;
//  OCTET_STRING_fromBuf(&ie->value.choice.Source_ToTarget_TransparentContainer,
//		  handover_req_pP->source_to_target_eutran_container->data, blength(handover_req_pP->source_to_target_eutran_container));
//  ASN_SEQUENCE_ADD(&out->protocolIEs.list, ie);
//
//  /* mandatory */
//  ie = (S1AP_HandoverRequestIEs_t *)calloc(1, sizeof(S1AP_HandoverRequestIEs_t));
//  ie->id = S1AP_ProtocolIE_ID_id_E_RABToBeSetupListHOReq;
//  ie->criticality = M2AP_Criticality_reject;
//  ie->value.present = S1AP_HandoverRequestIEs__value_PR_E_RABToBeSetupListHOReq;
//  ASN_SEQUENCE_ADD (&out->protocolIEs.list, ie);
//
//  S1AP_E_RABToBeSetupListHOReq_t	*e_rabtobesetuplisthoreq = &ie->value.choice.E_RABToBeSetupListHOReq;

  /** bstring of message will be destroyed outside of the ITTI message handler. */

  if (m2ap_mce_encode_pdu (&pdu, &buffer_p, &length) < 0) {
    // TODO: handle something
	OAILOG_ERROR (LOG_M2AP, "Failed to encode MBMS Session Stop Request. \n");
	/** We rely on the handover_notify timeout to remove the UE context. */
    OAILOG_FUNC_OUT (LOG_M2AP);
  }

  // todo: s1ap_generate_initiating_message will remove the things?
  OAILOG_NOTICE (LOG_M2AP, "Send M2AP_MBMS_SESSION_STOP_REQUEST message MCE_MBMS_M2AP_ID = " MCE_MBMS_M2AP_ID_FMT "\n",
		  INVALID_MBMS_SERVICE_INDEX);

  bstring b = blk2bstr(buffer_p, length);
  free(buffer_p);
  // todo: the next_sctp_stream is the one without incrementation?
  m2ap_mce_itti_send_sctp_request (&b, target_enb_ref->sctp_assoc_id, target_enb_ref->next_sctp_stream, INVALID_MBMS_SERVICE_INDEX);

  /*
   * Leave the state in as it is.
   * Not creating a UE-Reference towards the target-ENB.
   */
  OAILOG_FUNC_OUT (LOG_M2AP);
}

//------------------------------------------------------------------------------
void
m3ap_handle_mbms_session_update_request (
  const itti_m3ap_mbms_session_update_req_t * const mbms_session_update_req_pP)
{
  /*
   * MBMS-GW triggers an update of an MBMS Service on all eNBs, which are receiving it.
   */
  uint8_t                                *buffer_p = NULL;
  uint32_t                                length = 0;
  mbms_description_t               		 *mbms_ref = NULL;
  m2ap_enb_description_t                 *target_enb_ref = NULL;
  M2AP_M2AP_PDU_t                         pdu = {0};
//  S1AP_HandoverRequest_t		   		 *out;
//  S1AP_HandoverRequestIEs_t  			 *ie = NULL;

  OAILOG_FUNC_IN (LOG_M2AP);
  DevAssert (mbms_session_update_req_pP != NULL);

//  /*
//   * Based on the MCE_MBMS_M2AP_ID, you may or may not have a UE reference, we don't care. Just send HO_Request to the new eNB.
//   * Check that there exists an enb reference to the target-enb.
//   */
//  target_enb_ref = s1ap_is_enb_id_in_list(handover_req_pP->macro_enb_id);
//  if(!target_enb_ref){
//    OAILOG_ERROR (LOG_M2AP, "No target-enb could be found for enb-id %u. Handover Failed. \n",
//            handover_req_pP->macro_enb_id);
//    /**
//     * Send Handover Failure back (manually) to the MME.
//     * This will trigger an implicit detach if the UE is not REGISTERED yet (single MME S1AP HO), or will just send a HO-PREP-FAILURE to the MME (if the cause is not S1AP-SYSTEM-FAILURE).
//     */
//    MessageDef                             *message_p = NULL;
//    message_p = itti_alloc_new_message (TASK_S1AP, S1AP_HANDOVER_FAILURE);
//    AssertFatal (message_p != NULL, "itti_alloc_new_message Failed");
//    itti_s1ap_handover_failure_t *handover_failure_p = &message_p->ittiMsg.s1ap_handover_failure;
//    memset ((void *)&message_p->ittiMsg.s1ap_handover_failure, 0, sizeof (itti_s1ap_handover_failure_t));
//    /** Fill the S1AP Handover Failure elements per hand. */
//    handover_failure_p->mme_ue_s1ap_id = handover_req_pP->ue_id;
//    /** No need to remove any UE_Reference to the target_enb, not existing. */
//    itti_send_msg_to_task (TASK_MME_APP, INSTANCE_DEFAULT, message_p);
//    OAILOG_FUNC_OUT (LOG_M2AP);
//  }
//
//  /**
//   * UE Reference will only be created with a valid ENB_UE_S1AP_ID!
//   * We don't wan't to save 2 the UE reference twice in the hashmap, but only with a valid ENB_ID key.
//   * That's why create the UE Reference only with Handover Request Acknowledge.
//   * No timer will be created for Handover Request (not defined in the specification and no UE-Reference to the target-ENB exists yet.
//   *
//   * Target eNB could be found. Create a new ue_reference.
//   * This UE eNB Id has currently no known s1 association.
//   * * * * Create new UE context by associating new mme_ue_s1ap_id.
//   * * * * Update eNB UE list.
//   *
//   * todo: what to provide as enb_id?
//   */
//
//  /*
//   * Start the outcome response timer.
//   *
//   * * * * When time is reached, MME consider that procedure outcome has failed.
//   */
//  //     timer_setup(mme_config.s1ap_config.outcome_drop_timer_sec, 0, TASK_S1AP, INSTANCE_DEFAULT,
//  //                 TIMER_ONE_SHOT,
//  //                 NULL,
//  //                 &ue_ref->outcome_response_timer_id);
//  /*
//   * Insert the timer in the MAP of mme_ue_s1ap_id <-> timer_id
//   */
//  //     s1ap_timer_insert(ue_ref->mme_ue_s1ap_id, ue_ref->outcome_response_timer_id);
//  // todo: PSR if the state is handover, else just complete the message!
//  memset(&pdu, 0, sizeof(pdu));
//  pdu.present = M2AP_M2AP_PDU_PR_initiatingMessage;
//  pdu.choice.initiatingMessage.procedureCode = S1AP_ProcedureCode_id_HandoverResourceAllocation;
//  pdu.choice.initiatingMessage.criticality = M2AP_Criticality_ignore;
//  pdu.choice.initiatingMessage.value.present = S1AP_InitiatingMessage__value_PR_HandoverRequest;
//  out = &pdu.choice.initiatingMessage.value.choice.HandoverRequest;
//
//  /* mandatory */
//  ie = (S1AP_HandoverRequestIEs_t *)calloc(1, sizeof(S1AP_HandoverRequestIEs_t));
//  ie->id = S1AP_ProtocolIE_ID_id_MCE_MBMS_M2AP_ID;
//  ie->criticality = M2AP_Criticality_reject;
//  ie->value.present = S1AP_HandoverRequestIEs__value_PR_MCE_MBMS_M2AP_ID;
//  ie->value.choice.MCE_MBMS_M2AP_ID = handover_req_pP->ue_id;
//  ASN_SEQUENCE_ADD(&out->protocolIEs.list, ie);
//
//  /** Set Handover Type. */
//  ie = (S1AP_HandoverRequestIEs_t *)calloc(1, sizeof(S1AP_HandoverRequestIEs_t));
//  ie->id = S1AP_ProtocolIE_ID_id_HandoverType;
//  ie->criticality = M2AP_Criticality_reject;
//  ie->value.present = S1AP_HandoverRequestIEs__value_PR_HandoverType;
//  ie->value.choice.HandoverType = S1AP_HandoverType_intralte;
//  ASN_SEQUENCE_ADD(&out->protocolIEs.list, ie);
//
//  /** Set Id-Cause. */
//  ie = (S1AP_HandoverRequestIEs_t *)calloc(1, sizeof(S1AP_HandoverRequestIEs_t));
//  ie->id = S1AP_ProtocolIE_ID_id_HO_Cause;
//  ie->criticality = M2AP_Criticality_reject;
//  ie->value.present = S1AP_HandoverRequestIEs__value_PR_Cause;
//  ie->value.choice.Cause.present = S1AP_Cause_PR_radioNetwork;
//  ie->value.choice.Cause.choice.radioNetwork = S1AP_CauseRadioNetwork_handover_desirable_for_radio_reason;
//  ASN_SEQUENCE_ADD(&out->protocolIEs.list, ie);
//
//  /*
//   * uEaggregateMaximumBitrateDL and uEaggregateMaximumBitrateUL expressed in term of bits/sec
//   */
//  ie = (S1AP_HandoverRequestIEs_t *)calloc(1, sizeof(S1AP_HandoverRequestIEs_t));
//  ie->id = S1AP_ProtocolIE_ID_id_uEaggregateMaximumBitrate;
//  ie->criticality = M2AP_Criticality_reject;
//  ie->value.present = S1AP_HandoverRequestIEs__value_PR_UEAggregateMaximumBitrate;
//  asn_uint642INTEGER (&ie->value.choice.UEAggregateMaximumBitrate.uEaggregateMaximumBitRateDL, handover_req_pP->ambr.br_dl);
//  asn_uint642INTEGER (&ie->value.choice.UEAggregateMaximumBitrate.uEaggregateMaximumBitRateUL, handover_req_pP->ambr.br_ul);
//  ASN_SEQUENCE_ADD(&out->protocolIEs.list, ie);
//
//  /** Set the UE security capabilities. */
//  ie = (S1AP_HandoverRequestIEs_t *)calloc(1, sizeof(S1AP_HandoverRequestIEs_t));
//  ie->id = S1AP_ProtocolIE_ID_id_UESecurityCapabilities;
//  ie->criticality = M2AP_Criticality_reject;
//  ie->value.present = S1AP_HandoverRequestIEs__value_PR_UESecurityCapabilities;
//  S1AP_UESecurityCapabilities_t	*const  ue_security_capabilities = &ie->value.choice.UESecurityCapabilities;
//  ue_security_capabilities->encryptionAlgorithms.buf = calloc(1, sizeof(uint16_t));
//  memcpy(ue_security_capabilities->encryptionAlgorithms.buf, &handover_req_pP->security_capabilities_encryption_algorithms, sizeof(uint16_t));
//  ue_security_capabilities->encryptionAlgorithms.size = 2;
//  ue_security_capabilities->encryptionAlgorithms.bits_unused = 0;
//  OAILOG_DEBUG (LOG_M2AP, "security_capabilities_encryption_algorithms 0x%04X\n", handover_req_pP->security_capabilities_encryption_algorithms);
//
//  ue_security_capabilities->integrityProtectionAlgorithms.buf = calloc(1, sizeof(uint16_t));
//  memcpy(ue_security_capabilities->integrityProtectionAlgorithms.buf, &handover_req_pP->security_capabilities_integrity_algorithms, sizeof(uint16_t));
//  ue_security_capabilities->integrityProtectionAlgorithms.size = 2;
//  ue_security_capabilities->integrityProtectionAlgorithms.bits_unused = 0;
//  OAILOG_DEBUG (LOG_M2AP, "security_capabilities_integrity_algorithms 0x%04X\n", handover_req_pP->security_capabilities_integrity_algorithms);
//  ASN_SEQUENCE_ADD(&out->protocolIEs.list, ie);
//
//  /** Add the security context. */
//  ie = (S1AP_HandoverRequestIEs_t *)calloc(1, sizeof(S1AP_HandoverRequestIEs_t));
//  ie->id = S1AP_ProtocolIE_ID_id_SecurityContext;
//  ie->criticality = M2AP_Criticality_reject;
//  ie->value.present = S1AP_HandoverRequestIEs__value_PR_SecurityContext;
//  if (handover_req_pP->nh) {
//	ie->value.choice.SecurityContext.nextHopParameter.buf = calloc (AUTH_NH_SIZE, sizeof(uint8_t));
//	memcpy (ie->value.choice.SecurityContext.nextHopParameter.buf, handover_req_pP->nh, AUTH_NH_SIZE);
//	ie->value.choice.SecurityContext.nextHopParameter.size = AUTH_NH_SIZE;
//  } else {
//	  OAILOG_DEBUG (LOG_M2AP, "No nh \n");
//	  ie->value.choice.SecurityContext.nextHopParameter.buf = NULL;
//	  ie->	value.choice.SecurityContext.nextHopParameter.size = 0;
//  }
//  ie->value.choice.SecurityContext.nextHopParameter.bits_unused = 0;
//  ie->value.choice.SecurityContext.nextHopChainingCount = handover_req_pP->ncc;
//  ASN_SEQUENCE_ADD(&out->protocolIEs.list, ie);
//
//  /*
//   * E-UTRAN Target-ToSource Transparent Container.
//   */
//  ie = (S1AP_HandoverRequestIEs_t *)calloc(1, sizeof(S1AP_HandoverRequestIEs_t));
//  ie->id = S1AP_ProtocolIE_ID_id_Source_ToTarget_TransparentContainer;
//  ie->criticality = M2AP_Criticality_reject;
//  ie->value.present = S1AP_HandoverRequestIEs__value_PR_Source_ToTarget_TransparentContainer;
//  OCTET_STRING_fromBuf(&ie->value.choice.Source_ToTarget_TransparentContainer,
//		  handover_req_pP->source_to_target_eutran_container->data, blength(handover_req_pP->source_to_target_eutran_container));
//  ASN_SEQUENCE_ADD(&out->protocolIEs.list, ie);
//
//  /* mandatory */
//  ie = (S1AP_HandoverRequestIEs_t *)calloc(1, sizeof(S1AP_HandoverRequestIEs_t));
//  ie->id = S1AP_ProtocolIE_ID_id_E_RABToBeSetupListHOReq;
//  ie->criticality = M2AP_Criticality_reject;
//  ie->value.present = S1AP_HandoverRequestIEs__value_PR_E_RABToBeSetupListHOReq;
//  ASN_SEQUENCE_ADD (&out->protocolIEs.list, ie);
//
//  S1AP_E_RABToBeSetupListHOReq_t	*e_rabtobesetuplisthoreq = &ie->value.choice.E_RABToBeSetupListHOReq;

  /** bstring of message will be destroyed outside of the ITTI message handler. */

  if (m2ap_mce_encode_pdu (&pdu, &buffer_p, &length) < 0) {
    // TODO: handle something
	OAILOG_ERROR (LOG_M2AP, "Failed to encode MBMS Session Update Request. \n");
	/** We rely on the handover_notify timeout to remove the UE context. */
    OAILOG_FUNC_OUT (LOG_M2AP);
  }

  // todo: s1ap_generate_initiating_message will remove the things?
  OAILOG_NOTICE (LOG_M2AP, "Send M2AP_MBMS_SESSION_UPDATE_REQUEST message MCE_MBMS_M2AP_ID = " MCE_MBMS_M2AP_ID_FMT "\n",
              INVALID_MBMS_SERVICE_INDEX);

  bstring b = blk2bstr(buffer_p, length);
  free(buffer_p);
  // todo: the next_sctp_stream is the one without incrementation?
  m2ap_mce_itti_send_sctp_request (&b, target_enb_ref->sctp_assoc_id, target_enb_ref->next_sctp_stream, INVALID_MBMS_SERVICE_INDEX);

  /*
   * Leave the state in as it is.
   * Not creating a UE-Reference towards the target-ENB.
   */
  OAILOG_FUNC_OUT (LOG_M2AP);
}
//
////------------------------------------------------------------------------------
//void
//m2ap_handle_mce_mbms_id_notification (
//  const itti_mce_app_m2ap_mme_mbms_id_notification_t * const notification_p)
//{
//
//  OAILOG_FUNC_IN (LOG_M2AP);
//  DevAssert (notification_p != NULL);
//  m2ap_notified_new_mbms_mme_m2ap_id_association (
//                          notification_p->sctp_assoc_id, notification_p->enb_mbms_m2ap_id, notification_p->mce_mbms_m2ap_id);
//  OAILOG_FUNC_OUT (LOG_M2AP);
//}
