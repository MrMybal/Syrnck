package main

import "errors"

// Opaque reception never scans a working directory or creates an authored
// plaintext revision. Refuse mixed roles before opening a Store or a listener.
func validateReceiverConfiguration(config configuration) error {
	if !config.ReceiveEncrypted {
		return nil
	}
	if config.Storage == "" {
		return errors.New("receive_encrypted requires an explicit initialized storage directory")
	}
	if config.Directory != "" || config.Workspace != "" || config.UsePermissions || config.PreserveCheckoutEdits || config.ConflictCopies || config.ResumeDownloads {
		return errors.New("receive_encrypted cannot use workspace, directory or plaintext working-file options")
	}
	if config.UntrustedPeer || config.FolderPasswordEnv != "" || config.Author != nil || config.SigningKeyEnv != "" || config.SigningKeyFile != "" || config.SigningKeyPasswordEnv != "" {
		return errors.New("receive_encrypted cannot use a folder password, untrusted_peer or revision author/signing keys")
	}
	return nil
}
