#include "locale/message_code_map.h"

#include <QHash>

namespace aegra::desktop {
namespace {

[[nodiscard]] const QHash<QString, QString>& catalog() {
    static const QHash<QString, QString> kCatalog{
        {QStringLiteral("service.ready"), QStringLiteral("aegra.service.message.ready")},
        {QStringLiteral("service.disconnected"),
         QStringLiteral("aegra.error.service.disconnected")},
        {QStringLiteral("service.connect_failed"),
         QStringLiteral("aegra.error.service.connect_failed")},
        {QStringLiteral("service.protocol_invalid"),
         QStringLiteral("aegra.error.service.protocol_invalid")},
        {QStringLiteral("service.request_timeout"),
         QStringLiteral("aegra.error.service.request_timeout")},
        {QStringLiteral("service.send_failed"), QStringLiteral("aegra.error.service.send_failed")},
        {QStringLiteral("repository.not_configured"),
         QStringLiteral("aegra.repository.status.not_configured")},
        {QStringLiteral("repository.catalog_ready"),
         QStringLiteral("aegra.repository.status.catalog_ready")},
        {QStringLiteral("repository.query_failed"),
         QStringLiteral("aegra.error.repository.query_failed")},
        {QStringLiteral("recovery_point.layout_failed"),
         QStringLiteral("aegra.error.recovery_point.layout_failed")},
        {QStringLiteral("job.queued"), QStringLiteral("aegra.task.state.queued")},
        {QStringLiteral("job.running"), QStringLiteral("aegra.task.state.running")},
        {QStringLiteral("job.progress"), QStringLiteral("aegra.task.state.running")},
        {QStringLiteral("job.cancelling"), QStringLiteral("aegra.task.state.cancelling")},
        {QStringLiteral("job.succeeded"), QStringLiteral("aegra.task.state.succeeded")},
        {QStringLiteral("job.failed"), QStringLiteral("aegra.task.state.failed")},
        {QStringLiteral("job.cancelled"), QStringLiteral("aegra.task.state.cancelled")},
        {QStringLiteral("job.interrupted"), QStringLiteral("aegra.task.state.interrupted")},
        {QStringLiteral("job.deadline_exceeded"),
         QStringLiteral("aegra.error.job.deadline_exceeded")},
        {QStringLiteral("job.query_failed"), QStringLiteral("aegra.error.job.query_failed")},
        {QStringLiteral("inventory.query_failed"),
         QStringLiteral("aegra.error.inventory.query_failed")},
        {QStringLiteral("connection.query_failed"),
         QStringLiteral("aegra.error.connection.query_failed")},
        {QStringLiteral("backup.command_failed"),
         QStringLiteral("aegra.error.backup.command_failed")},
        {QStringLiteral("backup.preflight_failed"),
         QStringLiteral("aegra.error.backup.preflight_failed")},
        {QStringLiteral("backup.repository_unavailable"),
         QStringLiteral("aegra.error.backup.repository_unavailable")},
        {QStringLiteral("backup.source_not_selectable"),
         QStringLiteral("aegra.error.backup.source_not_selectable")},
        {QStringLiteral("backup.source_not_found"),
         QStringLiteral("aegra.error.backup.source_not_found")},
        {QStringLiteral("backup.worker_unavailable"),
         QStringLiteral("aegra.error.backup.worker_unavailable")},
        {QStringLiteral("backup.idempotency_conflict"),
         QStringLiteral("aegra.error.backup.idempotency_conflict")},
        {QStringLiteral("backup.parent_unavailable"),
         QStringLiteral("aegra.error.backup.parent_unavailable")},
        {QStringLiteral("restore.preflight_ready"),
         QStringLiteral("aegra.restore.preflight_ready")},
        {QStringLiteral("restore.preflight_failed"),
         QStringLiteral("aegra.error.restore.preflight_failed")},
        {QStringLiteral("restore.command_failed"),
         QStringLiteral("aegra.error.restore.command_failed")},
        {QStringLiteral("pe_restore.preflight_failed"),
         QStringLiteral("aegra.error.pe_restore.preflight_failed")},
        {QStringLiteral("pe_restore.command_failed"),
         QStringLiteral("aegra.error.pe_restore.command_failed")},
        {QStringLiteral("pe_restore.payload_missing"),
         QStringLiteral("aegra.error.pe_restore.payload_missing")},
        {QStringLiteral("pe_restore.pending_exists"),
         QStringLiteral("aegra.error.pe_restore.pending_exists")},
        {QStringLiteral("pe_restore.succeeded"),
         QStringLiteral("aegra.event.pe_restore.succeeded")},
        {QStringLiteral("pe_restore.failed"),
         QStringLiteral("aegra.event.pe_restore.failed")},
        {QStringLiteral("pe_restore.cancelled"),
         QStringLiteral("aegra.event.pe_restore.cancelled")},
        {QStringLiteral("restore.completed"),
         QStringLiteral("aegra.restore.summary.finished")},
        {QStringLiteral("restore.writing"), QStringLiteral("aegra.task.state.running")},
        {QStringLiteral("restore.running"), QStringLiteral("aegra.task.state.running")},
        {QStringLiteral("restore.preflight_invalid"),
         QStringLiteral("aegra.error.restore.preflight_failed")},
        {QStringLiteral("restore.system_target_requires_pe"),
         QStringLiteral("aegra.error.restore.system_target_requires_pe")},
        {QStringLiteral("restore.target_too_small"),
         QStringLiteral("aegra.error.restore.target_too_small")},
        {QStringLiteral("restore.shrink_provisional"),
         QStringLiteral("aegra.error.restore.shrink_provisional")},
        {QStringLiteral("restore.shrink_plan_ready"),
         QStringLiteral("aegra.error.restore.shrink_plan_ready")},
        {QStringLiteral("restore.shrink_not_ntfs"),
         QStringLiteral("aegra.error.restore.shrink_not_ntfs")},
        {QStringLiteral("restore.shrink_sector_mismatch"),
         QStringLiteral("aegra.error.restore.shrink_sector_mismatch")},
        {QStringLiteral("restore.shrink_below_minimum"),
         QStringLiteral("aegra.error.restore.shrink_below_minimum")},
        {QStringLiteral("restore.shrink_unsupported_layout"),
         QStringLiteral("aegra.error.restore.shrink_unsupported_layout")},
        {QStringLiteral("restore.shrink_scratch_insufficient"),
         QStringLiteral("aegra.error.restore.shrink_scratch_insufficient")},
        {QStringLiteral("restore.shrink_plan_changed"),
         QStringLiteral("aegra.error.restore.shrink_plan_changed")},
        {QStringLiteral("restore.shrink_plan_corrupt"),
         QStringLiteral("aegra.error.restore.shrink_plan_corrupt")},
        {QStringLiteral("restore.shrink_target_incomplete"),
         QStringLiteral("aegra.error.restore.shrink_target_incomplete")},
        {QStringLiteral("restore.shrink_postcheck_failed"),
         QStringLiteral("aegra.error.restore.shrink_postcheck_failed")},
        {QStringLiteral("restore.shrink_commit_outcome_unknown"),
         QStringLiteral("aegra.error.restore.shrink_commit_outcome_unknown")},
        {QStringLiteral("restore.shrink_analyze_failed"),
         QStringLiteral("aegra.error.restore.shrink_analyze_failed")},
        {QStringLiteral("service.capability_unavailable"),
         QStringLiteral("aegra.error.service.capability_unavailable")},
        {QStringLiteral("mount.command_failed"),
         QStringLiteral("aegra.error.mount.command_failed")},
        {QStringLiteral("mount.list_failed"), QStringLiteral("aegra.error.mount.list_failed")},
        {QStringLiteral("mount.host_unavailable"),
         QStringLiteral("aegra.error.mount.host_unavailable")},
        {QStringLiteral("mount.already_mounted"),
         QStringLiteral("aegra.error.mount.already_mounted")},
        {QStringLiteral("mount.dokan_unavailable"),
         QStringLiteral("aegra.error.mount.dokan_unavailable")},
        {QStringLiteral("mount.disk_not_found"),
         QStringLiteral("aegra.error.mount.disk_not_found")},
        {QStringLiteral("mount.host_failed"), QStringLiteral("aegra.error.mount.host_failed")},
        {QStringLiteral("service.request_failed"),
         QStringLiteral("aegra.error.service.request_failed")},
        {QStringLiteral("repository.locator_exists"),
         QStringLiteral("aegra.error.repository.locator_exists")},
        {QStringLiteral("repository.import_available"),
         QStringLiteral("aegra.error.repository.import_available")},
        {QStringLiteral("repository.location_occupied"),
         QStringLiteral("aegra.error.repository.location_occupied")},
        {QStringLiteral("repository.network_connect_failed"),
         QStringLiteral("aegra.repository.network_connect_failed")},
        {QStringLiteral("repository.network_unreachable"),
         QStringLiteral("aegra.repository.network_unreachable")},
        {QStringLiteral("repository.network_credentials_rejected"),
         QStringLiteral("aegra.repository.network_credentials_rejected")},
        {QStringLiteral("repository.network_access_denied"),
         QStringLiteral("aegra.repository.network_access_denied")},
        {QStringLiteral("repository.network_share_not_found"),
         QStringLiteral("aegra.repository.network_share_not_found")},
        {QStringLiteral("repository.network_credential_conflict"),
         QStringLiteral("aegra.repository.network_credential_conflict")},
        {QStringLiteral("repository.connection_failed"),
         QStringLiteral("aegra.repository.connection_failed")},
        {QStringLiteral("repository.network_path_invalid"),
         QStringLiteral("aegra.repository.network_path_invalid")},
        {QStringLiteral("repository.storage_root_invalid"),
         QStringLiteral("aegra.repository.storage_root_invalid")},
        {QStringLiteral("repository.storage_access_denied"),
         QStringLiteral("aegra.repository.storage_access_denied")},
        {QStringLiteral("repository.storage_path_not_found"),
         QStringLiteral("aegra.repository.storage_path_not_found")},
        {QStringLiteral("repository.storage_io_failed"),
         QStringLiteral("aegra.repository.storage_io_failed")},
        {QStringLiteral("repository.descriptor_invalid"),
         QStringLiteral("aegra.repository.descriptor_invalid")},
        {QStringLiteral("job.cancel_failed"), QStringLiteral("aegra.error.job.cancel_failed")},
        {QStringLiteral("command.accepted"),
         QStringLiteral("aegra.service.message.command_accepted")},
        {QStringLiteral("command.replayed"),
         QStringLiteral("aegra.service.message.command_replayed")},
        {QStringLiteral("control_plane.ready"), QStringLiteral("aegra.service.message.ready")},
        {QStringLiteral("file_browse.query_failed"),
         QStringLiteral("aegra.error.file_browse.query_failed")},
        {QStringLiteral("file_browse.token_limit"),
         QStringLiteral("aegra.error.file_browse.token_limit")},
        {QStringLiteral("file_browse.token_invalid"),
         QStringLiteral("aegra.error.file_browse.token_invalid")},
        {QStringLiteral("file_recover.query_failed"),
         QStringLiteral("aegra.error.file_recover.query_failed")},
        {QStringLiteral("file_recover.credential_required"),
         QStringLiteral("aegra.error.file_recover.credential_required")},
        {QStringLiteral("file_recover.credential_failed"),
         QStringLiteral("aegra.error.file_recover.credential_failed")},
        {QStringLiteral("file_recover.corrupt"),
         QStringLiteral("aegra.error.file_recover.corrupt")},
        {QStringLiteral("file_recover.catalog_only"),
         QStringLiteral("aegra.error.file_recover.catalog_only")},
        {QStringLiteral("file_recover.parent_missing"),
         QStringLiteral("aegra.error.file_recover.parent_missing")},
        {QStringLiteral("file_recover.parent_reference_invalid"),
         QStringLiteral("aegra.error.file_recover.parent_reference_invalid")},
        {QStringLiteral("file_recover.chain_depth_limit"),
         QStringLiteral("aegra.error.file_recover.chain_depth_limit")},
        {QStringLiteral("file_recover.token_invalid"),
         QStringLiteral("aegra.error.file_recover.token_invalid")},
        {QStringLiteral("file_backup.incremental_downgraded_full"),
         QStringLiteral("aegra.error.file_backup.incremental_downgraded_full")},
        {QStringLiteral("file_backup.parent_chain_invalid"),
         QStringLiteral("aegra.error.file_backup.parent_chain_invalid")},
        {QStringLiteral("file_backup.selection_fingerprint_mismatch"),
         QStringLiteral("aegra.error.file_backup.selection_fingerprint_mismatch")},
        {QStringLiteral("file_backup.metadata_baseline_invalid"),
         QStringLiteral("aegra.error.file_backup.metadata_baseline_invalid")},
        {QStringLiteral("file_backup.unsupported_incremental"),
         QStringLiteral("aegra.error.file_backup.unsupported_incremental")},
        {QStringLiteral("file_source.unsupported_reparse"),
         QStringLiteral("aegra.error.file_source.unsupported_reparse")},
        {QStringLiteral("file_source.unsupported_hard_link"),
         QStringLiteral("aegra.error.file_source.unsupported_hard_link")},
        {QStringLiteral("file_source.unsupported_sparse"),
         QStringLiteral("aegra.error.file_source.unsupported_sparse")},
        {QStringLiteral("file_source.unsupported_ads"),
         QStringLiteral("aegra.error.file_source.unsupported_ads")},
        {QStringLiteral("file_source.unsupported_unc"),
         QStringLiteral("aegra.error.file_source.unsupported_unc")},
        {QStringLiteral("file_source.unsupported_filesystem"),
         QStringLiteral("aegra.error.file_source.unsupported_filesystem")},
        {QStringLiteral("file_source.unsupported_efs"),
         QStringLiteral("aegra.error.file_source.unsupported_efs")},
        {QStringLiteral("file_source.unsupported_cloud_placeholder"),
         QStringLiteral("aegra.error.file_source.unsupported_cloud_placeholder")},
        {QStringLiteral("file_restore.preflight_ok"),
         QStringLiteral("aegra.error.file_restore.preflight_ok")},
        {QStringLiteral("file_restore.preflight_failed"),
         QStringLiteral("aegra.error.file_restore.preflight_failed")},
        {QStringLiteral("file_restore.preflight_expired"),
         QStringLiteral("aegra.error.file_restore.preflight_expired")},
        {QStringLiteral("file_restore.preflight_consumed"),
         QStringLiteral("aegra.error.file_restore.preflight_consumed")},
        {QStringLiteral("file_restore.command_failed"),
         QStringLiteral("aegra.error.file_restore.command_failed")},
        {QStringLiteral("file_restore.selection_limit"),
         QStringLiteral("aegra.error.file_restore.selection_limit")},
        {QStringLiteral("file_restore.target_capability_missing"),
         QStringLiteral("aegra.error.file_restore.target_capability_missing")},
        {QStringLiteral("file_restore.target_file_too_large"),
         QStringLiteral("aegra.error.file_restore.target_file_too_large")},
        {QStringLiteral("file_restore.target_not_directory"),
         QStringLiteral("aegra.error.file_restore.target_not_directory")},
        {QStringLiteral("file_restore.target_reparse_escape"),
         QStringLiteral("aegra.error.file_restore.target_reparse_escape")},
        {QStringLiteral("file_restore.target_collision"),
         QStringLiteral("aegra.error.file_restore.target_collision")},
        {QStringLiteral("file_restore.rename_exhausted"),
         QStringLiteral("aegra.error.file_restore.rename_exhausted")},
        {QStringLiteral("file_restore.target_full"),
         QStringLiteral("aegra.error.file_restore.target_full")},
        {QStringLiteral("file_restore.partial"),
         QStringLiteral("aegra.error.file_restore.partial")},
        {QStringLiteral("file_restore.failed_before_write"),
         QStringLiteral("aegra.error.file_restore.failed_before_write")},
        {QStringLiteral("file_restore.original_location_unsupported"),
         QStringLiteral("aegra.error.file_restore.original_location_unsupported")},
        {QStringLiteral("file_restore.system_directory_unsupported"),
         QStringLiteral("aegra.error.file_restore.system_directory_unsupported")},
        {QStringLiteral("file_restore.completed"),
         QStringLiteral("aegra.error.file_restore.completed")},
        {QStringLiteral("service.content_kind_mismatch"),
         QStringLiteral("aegra.error.file_restore.content_kind_mismatch")},
        {QStringLiteral("bootcheck.provider_unavailable"),
         QStringLiteral("aegra.error.bootcheck.provider_unavailable")},
        {QStringLiteral("bootcheck.virtualbox_hyperv_conflict"),
         QStringLiteral("aegra.error.bootcheck.virtualbox_hyperv_conflict")},
        {QStringLiteral("bootcheck.virtualbox_no_hardware_virt"),
         QStringLiteral("aegra.error.bootcheck.virtualbox_no_hardware_virt")},
        {QStringLiteral("bootcheck.volume_set_required"),
         QStringLiteral("aegra.error.bootcheck.volume_set_required")},
        {QStringLiteral("bootcheck.archive_missing"),
         QStringLiteral("aegra.error.bootcheck.archive_missing")},
        {QStringLiteral("bootcheck.archive_credential_unavailable"),
         QStringLiteral("aegra.error.bootcheck.archive_credential_unavailable")},
        {QStringLiteral("bootcheck.archive_corrupt"),
         QStringLiteral("aegra.error.bootcheck.archive_corrupt")},
        {QStringLiteral("bootcheck.archive_open_failed"),
         QStringLiteral("aegra.error.bootcheck.archive_open_failed")},
        {QStringLiteral("bootcheck.source_not_system_disk"),
         QStringLiteral("aegra.error.bootcheck.source_not_system_disk")},
        {QStringLiteral("bootcheck.unsupported_boot_profile"),
         QStringLiteral("aegra.error.bootcheck.unsupported_boot_profile")},
        {QStringLiteral("bootcheck.vmdk_present_failed"),
         QStringLiteral("aegra.error.bootcheck.vmdk_present_failed")},
        {QStringLiteral("bootcheck.vm_create_failed"),
         QStringLiteral("aegra.error.bootcheck.vm_create_failed")},
        {QStringLiteral("bootcheck.vm_start_failed"),
         QStringLiteral("aegra.error.bootcheck.vm_start_failed")},
        {QStringLiteral("bootcheck.guest_powered_off"),
         QStringLiteral("aegra.error.bootcheck.guest_powered_off")},
        {QStringLiteral("bootcheck.boot_not_confirmed"),
         QStringLiteral("aegra.error.bootcheck.boot_not_confirmed")},
        {QStringLiteral("bootcheck.overlay_full"),
         QStringLiteral("aegra.error.bootcheck.overlay_full")},
        {QStringLiteral("bootcheck.cancelled"),
         QStringLiteral("aegra.error.bootcheck.cancelled")},
        {QStringLiteral("bootcheck.cleanup_incomplete"),
         QStringLiteral("aegra.error.bootcheck.cleanup_incomplete")},
        {QStringLiteral("bootcheck.host_failed"),
         QStringLiteral("aegra.error.bootcheck.host_failed")},
        {QStringLiteral("bootcheck.request_rejected"),
         QStringLiteral("aegra.error.bootcheck.request_rejected")},
        {QStringLiteral("post_backup.boot_check_unavailable"), QStringLiteral("aegra.error.bootcheck.host_unavailable")},
        {QStringLiteral("post_backup.boot_check_interrupted"), QStringLiteral("aegra.error.bootcheck.interrupted")},
        {QStringLiteral("post_backup.boot_check_timeout"), QStringLiteral("aegra.error.bootcheck.timeout")},
        {QStringLiteral("post_backup.boot_check_dispatch_failed"), QStringLiteral("aegra.error.bootcheck.dispatch_failed")},
        {QStringLiteral("post_backup.boot_check_host_failed"), QStringLiteral("aegra.error.bootcheck.host_failed")},
        {QStringLiteral("verify.archive_missing"),
         QStringLiteral("aegra.error.verify.archive_missing")},
        {QStringLiteral("verify.source_unavailable"),
         QStringLiteral("aegra.error.verify.source_unavailable")},
        {QStringLiteral("verify.corrupt"), QStringLiteral("aegra.error.verify.corrupt")},
        {QStringLiteral("verify.credential_unavailable"),
         QStringLiteral("aegra.error.verify.credential_unavailable")},
        {QStringLiteral("verify.cancelled"), QStringLiteral("aegra.error.verify.cancelled")},
        {QStringLiteral("verify.failed"), QStringLiteral("aegra.error.verify.failed")},
        {QStringLiteral("verify.invalid_request"),
         QStringLiteral("aegra.error.verify.invalid_request")},
    };
    return kCatalog;
}

} // namespace

QString translation_id_for_message_code(const QString& message_code) {
    const auto it = catalog().constFind(message_code);
    if (it == catalog().cend()) {
        return QStringLiteral("aegra.error.unknown");
    }
    return *it;
}

[[maybe_unused]] void keep_shrink_message_translation_ids() {
    //% "Smaller target needs NTFS shrink analysis before restore can start"
    qtTrId("aegra.error.restore.shrink_provisional");
    //% "NTFS shrink plan is ready for confirmation"
    qtTrId("aegra.error.restore.shrink_plan_ready");
    //% "Shrink restore requires an NTFS source volume"
    qtTrId("aegra.error.restore.shrink_not_ntfs");
    //% "Source and target sector geometry are incompatible for shrink restore"
    qtTrId("aegra.error.restore.shrink_sector_mismatch");
    //% "Target volume is smaller than the exact minimum required for shrink restore"
    qtTrId("aegra.error.restore.shrink_below_minimum");
    //% "This NTFS layout is not supported for shrink restore"
    qtTrId("aegra.error.restore.shrink_unsupported_layout");
    //% "Not enough scratch space to complete NTFS shrink restore"
    qtTrId("aegra.error.restore.shrink_scratch_insufficient");
    //% "Source, target, or shrink plan changed; analyze again"
    qtTrId("aegra.error.restore.shrink_plan_changed");
    //% "Shrink plan is corrupt or incomplete; analyze again"
    qtTrId("aegra.error.restore.shrink_plan_corrupt");
    //% "Restore stopped before commit; target is incomplete and requires a full retry"
    qtTrId("aegra.error.restore.shrink_target_incomplete");
    //% "Post-restore volume check failed; do not treat the target as healthy"
    qtTrId("aegra.error.restore.shrink_postcheck_failed");
    //% "Boot commit outcome is unknown; verify the target before reuse"
    qtTrId("aegra.error.restore.shrink_commit_outcome_unknown");
    //% "NTFS shrink analysis failed"
    qtTrId("aegra.error.restore.shrink_analyze_failed");
    //% "This Service capability is not available"
    qtTrId("aegra.error.service.capability_unavailable");
    //% "Target is too small for this restore"
    qtTrId("aegra.error.restore.target_too_small");
    //% "Offline restore payload is missing: %1. Reinstall the application and try again."
    qtTrId("aegra.error.pe_restore.payload_missing");
    //% "An offline restore is already prepared and waiting for restart"
    qtTrId("aegra.error.pe_restore.pending_exists");
    //% "The selected hypervisor is not available for boot check"
    qtTrId("aegra.error.bootcheck.provider_unavailable");
    //% "VirtualBox cannot start VMs because Hyper-V is using the CPU's virtualization. Enable the Windows feature 'Windows Hypervisor Platform', or switch boot check to Hyper-V."
    qtTrId("aegra.error.bootcheck.virtualbox_hyperv_conflict");
    //% "Hardware virtualization (VT-x/AMD-V) is not available. Enable it in the BIOS/UEFI firmware settings."
    qtTrId("aegra.error.bootcheck.virtualbox_no_hardware_virt");
    //% "Boot check requires a volume recovery point"
    qtTrId("aegra.error.bootcheck.volume_set_required");
    //% "Archive file does not exist"
    qtTrId("aegra.error.bootcheck.archive_missing");
    //% "Archive password is unavailable"
    qtTrId("aegra.error.bootcheck.archive_credential_unavailable");
    //% "Archive authentication failed"
    qtTrId("aegra.error.bootcheck.archive_corrupt");
    //% "Boot check could not open the archive. Check that the backup files still exist in the repository."
    qtTrId("aegra.error.bootcheck.archive_open_failed");
    //% "The recovery point does not contain a bootable system disk"
    qtTrId("aegra.error.bootcheck.source_not_system_disk");
    //% "The recovery point's boot configuration is not supported for boot check"
    qtTrId("aegra.error.bootcheck.unsupported_boot_profile");
    //% "Boot check could not present the virtual disk"
    qtTrId("aegra.error.bootcheck.vmdk_present_failed");
    //% "Boot check could not create the virtual machine"
    qtTrId("aegra.error.bootcheck.vm_create_failed");
    //% "Boot check could not start the virtual machine"
    qtTrId("aegra.error.bootcheck.vm_start_failed");
    //% "The virtual machine powered off before boot was confirmed"
    qtTrId("aegra.error.bootcheck.guest_powered_off");
    //% "The system did not boot within the time limit"
    qtTrId("aegra.error.bootcheck.boot_not_confirmed");
    //% "Boot check stopped because the temporary disk space was exhausted"
    qtTrId("aegra.error.bootcheck.overlay_full");
    //% "Boot check was cancelled"
    qtTrId("aegra.error.bootcheck.cancelled");
    //% "Boot check finished but could not fully remove its temporary virtual machine"
    qtTrId("aegra.error.bootcheck.cleanup_incomplete");
    //% "The boot check host process failed"
    qtTrId("aegra.error.bootcheck.host_failed");
    //% "The boot check request was rejected"
    qtTrId("aegra.error.bootcheck.request_rejected");
    //% "Boot check is not available on this computer"
    qtTrId("aegra.error.bootcheck.host_unavailable");
    //% "Boot check was interrupted by a service restart"
    qtTrId("aegra.error.bootcheck.interrupted");
    //% "Boot check exceeded its run time budget"
    qtTrId("aegra.error.bootcheck.timeout");
    //% "Boot check could not be started"
    qtTrId("aegra.error.bootcheck.dispatch_failed");
    //% "Archive file does not exist"
    qtTrId("aegra.error.verify.archive_missing");
    //% "Archive could not be opened. Check the path, repository connection, and file permissions."
    qtTrId("aegra.error.verify.source_unavailable");
    //% "Archive authentication failed"
    qtTrId("aegra.error.verify.corrupt");
    //% "Archive password is unavailable"
    qtTrId("aegra.error.verify.credential_unavailable");
    //% "Verification was cancelled"
    qtTrId("aegra.error.verify.cancelled");
    //% "Verification failed"
    qtTrId("aegra.error.verify.failed");
    //% "Verification request is invalid"
    qtTrId("aegra.error.verify.invalid_request");
}

QString localize_message_code(const QString& message_code) {
    const auto id = translation_id_for_message_code(message_code);
    if (id == QLatin1String("aegra.error.unknown")) {
        //% "Unexpected service response (%1)"
        auto text = qtTrId("aegra.error.unknown");
        // Without a loaded QM, qtTrId returns the ID which has no %1 placeholder.
        if (!text.contains(QLatin1String("%1"))) {
            text = QStringLiteral("Unexpected service response (%1)");
        }
        return text.arg(message_code);
    }
    auto text = qtTrId(id.toUtf8().constData());
    if (text == id) {
        // Keep a deterministic English fallback when the pack is missing this ID.
        return id;
    }
    return text;
}

} // namespace aegra::desktop
