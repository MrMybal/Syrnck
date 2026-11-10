package syncthing

import (
	"context"
	"errors"
	"net"
	"time"
)

// EncryptedReceiverServiceOptions reconnects one explicitly paired peer. A
// receiver can run independent services for several peers simultaneously.
// Connect obeys the same cancellation/transport ownership as FolderService.
type EncryptedReceiverServiceOptions struct {
	Session            EncryptedReceiverSessionOptions
	Connect            func(context.Context) (net.Conn, error)
	RetryMin, RetryMax time.Duration
	OfflineInterval    time.Duration
}

// OpenEncryptedReceiverService shares the resident reconnection lifecycle with
// workspace services, retaining completed cipher blocks across disconnections.
// It owns no receiver or idle Store; close services before closing the receiver.
func OpenEncryptedReceiverService(ctx context.Context, receiver *EncryptedReceiver, options EncryptedReceiverServiceOptions) (*FolderService, error) {
	if ctx == nil || options.Connect == nil {
		return nil, errors.New("syrnck BEP: service context and transport factory required")
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	prepared := FolderServiceOptions{Session: FolderSessionOptions{Device: options.Session.Device, PollInterval: options.Session.PollInterval, QueueBytes: options.Session.QueueBytes},
		Connect: options.Connect, RetryMin: options.RetryMin, RetryMax: options.RetryMax, OfflineInterval: options.OfflineInterval}
	freshHello := prepared.Session.Device.Hello.Timestamp == 0
	if err := prepareReceiverSessionOptions(receiver, &prepared.Session); err != nil {
		return nil, err
	}
	if _, err := receiver.Snapshot(ctx); err != nil {
		return nil, err
	}
	if freshHello {
		prepared.Session.Device.Hello.Timestamp = 0
	}
	return startFolderService(ctx, &FolderService{receiver: receiver}, prepared)
}

func (s *FolderService) openSession(ctx context.Context, stream net.Conn) (*FolderSession, error) {
	if s.receiver != nil {
		options := s.options.Session
		return OpenEncryptedReceiverSession(ctx, s.receiver, stream, EncryptedReceiverSessionOptions{Device: options.Device, PollInterval: options.PollInterval, QueueBytes: options.QueueBytes})
	}
	return OpenFolderSession(ctx, s.folder, stream, s.options.Session)
}

func (s *FolderService) refresh() (FolderStatus, error) {
	if s.receiver != nil {
		snapshot, err := s.receiver.Snapshot(s.ctx)
		return FolderStatus{State: "waiting_for_source", Catalog: snapshot.Catalog.ID}, err
	}
	return s.folder.Refresh(s.ctx)
}
