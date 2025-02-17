package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/cilium/little-vm-helper/pkg/images"
	"github.com/cilium/tetragon/pkg/vmtests"
	"github.com/sirupsen/logrus"
)

var (
	TetragonTesterBin   = "./tests/vmtests/tetragon-tester"
	TetragonTesterVmDir = "/sbin"
	TetragonTesterVmBin = filepath.Join(TetragonTesterVmDir, filepath.Base(TetragonTesterBin))
)

func buildFilesystemActions(fs []QemuFS, tmpDir string) ([]images.Action, error) {

	actions := make([]images.Action, 0, len(fs)+1)

	var b bytes.Buffer
	for _, fs := range fs {
		b.WriteString(fs.fstabEntry())
		act := images.Action{
			Op: &images.MkdirCommand{Dir: fs.vmMountpoint()},
		}
		actions = append(actions, act)
	}

	// NB: this is so that init can remount / rw
	b.WriteString("/dev/root\t/\text4\terrors=remount-ro\t0\t1\n")

	tmpFile := filepath.Join(tmpDir, "fstab")
	err := os.WriteFile(tmpFile, b.Bytes(), 0722)
	if err != nil {
		return nil, err
	}

	actions = append(actions, images.Action{
		Op: &images.CopyInCommand{
			LocalPath: tmpFile,
			RemoteDir: "/etc",
		},
	})

	return actions, nil
}

var tetragonTesterService = `
[Unit]
Description=Tetragon tester
After=network.target

[Service]
ExecStart=%s
Type=oneshot
# https://www.freedesktop.org/software/systemd/man/systemd.exec.html
# StandardOutput=file:%s
StandardOutput=tty
# StandardOutput=journal+console

[Install]
WantedBy=multi-user.target
`

func buildTesterService(rcnf *RunConf, tmpDir string) ([]images.Action, error) {
	service := fmt.Sprintf(tetragonTesterService, TetragonTesterVmBin, rcnf.testerOut)
	var b bytes.Buffer
	b.WriteString(service)

	tmpFile := filepath.Join(tmpDir, "tetragon-tester.service")
	err := os.WriteFile(tmpFile, b.Bytes(), 0722)
	if err != nil {
		return nil, err
	}

	actions := []images.Action{
		{Op: &images.CopyInCommand{
			LocalPath: tmpFile,
			RemoteDir: "/etc/systemd/system",
		}},
		/*
			{Op: &images.RunCommand{
				Cmd: "sed -i  's/^#LogColor=yes/LogColor=no/' /etc/systemd/system.conf",
			}},
		*/
	}

	enableTester := images.Action{Op: &images.RunCommand{Cmd: "systemctl enable tetragon-tester.service"}}
	actions = append(actions, enableTester)

	return actions, nil
}

func buildTesterActions(rcnf *RunConf, tmpDir string) ([]images.Action, error) {
	ret := []images.Action{
		{Op: &images.CopyInCommand{LocalPath: TetragonTesterBin, RemoteDir: "/sbin"}},
	}

	// NB: need to do this before we marshal the configuration
	if rcnf.btfFile != "" {
		ret = append(ret, images.Action{
			Op: &images.CopyInCommand{
				LocalPath: rcnf.btfFile,
				RemoteDir: "/boot/",
			},
		})

		baseName := filepath.Base(rcnf.btfFile)
		rcnf.testerConf.BTFFile = filepath.Join("/boot", baseName)
	}

	confB, err := json.MarshalIndent(&rcnf.testerConf, "", "    ")
	if err != nil {
		return nil, err
	}

	tmpConfFile := filepath.Join(tmpDir, filepath.Base(vmtests.ConfFile))
	remoteConfDir := filepath.Dir(vmtests.ConfFile)
	if err := os.WriteFile(tmpConfFile, confB, 0722); err != nil {
		return nil, err
	}

	ret = append(ret, images.Action{
		Op: &images.CopyInCommand{LocalPath: tmpConfFile, RemoteDir: remoteConfDir},
	})

	if !rcnf.useTetragonTesterInit && !rcnf.justBoot {
		acts, err := buildTesterService(rcnf, tmpDir)
		if err != nil {
			return nil, err
		}
		ret = append(ret, acts...)
	}

	return ret, nil
}

func buildTestImage(log *logrus.Logger, rcnf *RunConf) error {

	imagesDir, baseImage := filepath.Split(rcnf.baseFname)
	hostname := strings.TrimSuffix(rcnf.testImage, filepath.Ext(rcnf.testImage))

	tmpDir, err := os.MkdirTemp("", "tetragon-vmtests-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmpDir)

	fsActions, err := buildFilesystemActions(rcnf.filesystems, tmpDir)
	if err != nil {
		return err
	}

	testerActions, err := buildTesterActions(rcnf, tmpDir)
	if err != nil {
		return err
	}

	actions := []images.Action{
		{Op: &images.SetHostnameCommand{Hostname: hostname}},
		// NB: some of the tetragon tests expect a /usr/bin/cp
		{Op: &images.RunCommand{Cmd: "cp /bin/cp /usr/bin/cp"}},
		{Op: &images.AppendLineCommand{
			File: "/etc/sysctl.d/local.conf",
			Line: "kernel.panic_on_rcu_stall=1",
		}},
	}
	actions = append(actions, fsActions...)
	actions = append(actions, testerActions...)

	cnf := images.ImagesConf{
		Dir: imagesDir,
		// TODO: might be useful to modify the images builder so that
		// we can build this image using qemu-img -b
		Images: []images.ImgConf{{
			Name:    rcnf.testImage,
			Parent:  baseImage,
			Actions: actions,
		}},
	}

	forest, err := images.NewImageForest(&cnf, false)
	if err != nil {
		log.Fatal(err)
	}

	res := forest.BuildAllImages(&images.BuildConf{
		Log:          log,
		DryRun:       false,
		ForceRebuild: !rcnf.dontRebuildImage,
		MergeSteps:   true,
	})

	return res.Err()
}
