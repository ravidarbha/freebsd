
#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/dirent.h>
#include <sys/namei.h>

#include "virtfs_proto.h"
#include "virtfs.h"
#include "../client.h"


struct vop_vector virtfs_vnops;
static MALLOC_DEFINE(M_P9NODE, "virtfs_node", "virtfs node structures");

static int
virtfs_lookup(struct vop_cachedlookup_args *ap)
{
	/* direnode */
	struct vnode *dvp = ap->a_dvp;
	struct vnode **vpp = ap->a_vpp, *vp;
	struct componentname *cnp = ap->a_cnp;
	struct virtfs_node *dnp = dvp->v_data; /*dir p9_node */
	struct virtfs_session *p9s = dnp->virtfs_ses;
	struct mount *mp = p9s->virtfs_mount; /* Get the mount point */
	struct p9_fid *newfid = NULL;
	int error = 0;

	*vpp = NULL;

	/* Special case: lookup a directory from itself. */
	if (cnp->cn_namelen == 1 && *cnp->cn_nameptr == '.') {
		*vpp = dvp;
		vref(*vpp);
		return (0);
	}

	/* The clone has to be set to get a new fid */
	/* Here we are defaulting it to craete. Is that correct ? 
	 * Ideally we should be checking if its presnet in the cache.
	 * is not present, create the new fid and the qid from server
	 * to map it to the correct vnode */
	newfid = p9_client_walk(dnp->vfid,
	    cnp->cn_namelen, &cnp->cn_nameptr, 1);
	if (newfid != NULL) {
		int ltype = 0;

		if (cnp->cn_flags & ISDOTDOT) {
			ltype = VOP_ISLOCKED(dvp);
			VOP_UNLOCK(dvp, 0);
		}
		/* Vget gets the vp for the newly created vnode. Stick it to the virtfs_node too*/
		error = virtfs_vget(mp, newfid->fid, cnp->cn_lkflags, &vp);
		if (cnp->cn_flags & ISDOTDOT)
			vn_lock(dvp, ltype | LK_RETRY);
	}
	if (error == 0) {
		*vpp = vp;
		vref(*vpp);
	} 

	return (error);
}

/* We ll implement this once mount works fine .*/
static int
virtfs_create(struct vop_create_args *ap)
{
	return 0;
}

static int
virtfs_mknod(struct vop_mknod_args *ap)
{
	
	return 0;
}

static int
virtfs_open(struct vop_open_args *ap)
{
	int error = 0;
	struct virtfs_node *np = ap->a_vp->v_data;
	struct p9_fid *fid = np->vfid;
	struct p9_wstat *stat;
	size_t filesize;

	if (np->v_opens > 0) {
		np->v_opens++;
		return (0);
	}

	stat  = p9_client_stat(np->vfid);
	if (error != 0)
		return (error);

	if (ap->a_vp->v_type == VDIR) {
		if (np->vofid == NULL) {

			/*ofid is the open fid for this file.*/
			/* Note: Client_walk returns struct p9_fid* */
			np->vofid = p9_client_walk(np->vfid,
			     0, NULL, 1); /* Clone the fid here.*/
			if (error != 0) {
				np->vofid = NULL;
				return (error);
			}
		}
		fid = np->vofid;
	}

	filesize = np->inode.i_size;
	/* Use the newly created fid for the open.*/
	error = p9_client_open(fid, ap->a_mode);
	if (error == 0) {
		np->v_opens = 1;
		vnode_create_vobject(ap->a_vp, filesize, ap->a_td);
	}

	return (error);
}

static int
virtfs_close(struct vop_close_args *ap)
{
	struct virtfs_node *np = ap->a_vp->v_data;

	printf("%s(fid %d ofid %d opens %d)\n", __func__,
	    np->vfid->fid, np->vofid->fid, np->v_opens);
	np->v_opens--;
	if (np->v_opens == 0) {
		//virtfs_relfid(np->virtfs_ses, np->vofid);
		np->vofid = 0;
	}

	return (0);
}

static int
virtfs_getattr(struct vop_getattr_args *ap)
{
/*	struct virtfs_node *np = ap->a_vp->v_data;
	ap->a_vap = p9_client_stat(np->vfid);
	*/

	return 0;
}

#if 0
// make sure this version works first.
struct p9_wstat {
        uint16_t size;
        uint16_t type;
        uint32_t dev;
        struct p9_qid qid;
        uint32_t mode;
        uint32_t atime;
        uint32_t mtime;
        uint64_t length;
        char *name;
        char *uid;
        char *gid;
        char *muid;
        char *extension;        /* 9p2000.u extensions */
        uid_t n_uid;            /* 9p2000.u extensions */
        gid_t n_gid;            /* 9p2000.u extensions */
        uid_t n_muid;           /* 9p2000.u extensions */
};

#endif  

int
virtfs_stat_vnode(void *st, struct vnode *vp)
{
	struct virtfs_node *np = vp->v_data;
	struct virtfs_inode *inode = &np->inode;
	struct virtfs_session *v9s = np->virtfs_ses;

	if (virtfs_proto_dotl(v9s)) {

		struct p9_stat_dotl *stat = (struct p9_stat_dotl *)st;
		/* Just get the needed fields for now. We can add more later. */
                inode->i_mtime = stat->st_mtime_sec;
                inode->i_ctime = stat->st_ctime_sec;
                inode->i_uid = stat->st_uid;
                inode->i_gid = stat->st_gid;
                inode->i_blocks = stat->st_blocks;
		inode->i_mode = stat->st_mode;
	}
	else {
		struct p9_wstat *stat = (struct p9_wstat *)st;
		inode->i_mtime = stat->mtime;	
		inode->i_ctime = stat->atime;
 	        inode->i_uid = stat->n_uid; /* Make sure you copy the numeric */
                inode->i_gid = stat->n_gid;
		inode->i_mode = stat->mode;

		memcpy(&np->vqid, &stat->qid, sizeof(stat->qid));
#if 0
	uint64_t        i_blocks;
        uint64_t        i_size;
        uint64_t        i_ctime;
        uint64_t        i_mtime;
        uint32_t        i_uid;
        uint32_t        i_gid;
        uint16_t        i_mode;
        uint32_t        i_flags;	
#endif
		
	}

	return 0;
}

static int
virtfs_setattr(struct vop_setattr_args *ap)
{
	return 0;
}

static int
virtfs_read(struct vop_read_args *ap)
{
	return 0;
}

static int
virtfs_write(struct vop_write_args *ap)
{
	return 0;
}

static int
virtfs_fsync(struct vop_fsync_args *ap)
{
	return 0;
}

static int
virtfs_remove(struct vop_remove_args *ap)
{
	return 0; 
}

static int
virtfs_link(struct vop_link_args *ap)
{
	return 0;
}

static int
virtfs_rename(struct vop_rename_args *ap)
{
	return 0;
}

static int
virtfs_mkdir(struct vop_mkdir_args *ap)
{
	return 0;
}

static int
virtfs_rmdir(struct vop_rmdir_args *ap)
{
	return 0;
}

static int
virtfs_symlink(struct vop_symlink_args *ap)
{
	return 0;
}

/*
 * Minimum length for a directory entry: size of fixed size section of
 * struct dirent plus a 1 byte C string for the name.
 */
static int
virtfs_readdir(struct vop_readdir_args *ap)
{
	struct uio *uio = ap->a_uio;
        struct vnode *vp = ap->a_vp;
        struct dirent dirent;
        uint64_t diroffset = 0, transoffset = 0;
	uint64_t offset;
	struct virtfs_node *np = ap->a_vp->v_data;
        int error = 0;
	struct p9_wstat st;
	char *data = NULL;
	struct p9_fid *fid = NULL;
	struct p9_client *clnt = np->virtfs_ses->clnt;

	if (ap->a_uio->uio_iov->iov_len <= 0)
		return (EINVAL);

	if (vp->v_type != VDIR)
		return (ENOTDIR);

	error = 0;

	/* Our version of the readdir through the virtio. The data buf has the 
	 * data block information. Now parse through the buf and make the dirent.
	 */
	error = p9_client_readdir(np->vofid, (char *)data,
		clnt->msize, 0); /* The max size our client can handle */

	if (error) {
		return (EIO);
	}
#if 0
	struct p9_dirent {
        struct p9_qid qid;
        uint64_t d_off;
        unsigned char d_type;
        char d_name[256];
};
#endif // Directory entry 

	offset = 0;
	while (data && offset < clnt->msize) {

		/* Read and make sense out of the buffer in one dirent
		 * This is part of 9p protocol read.
		 */
		error = p9stat_read(fid->clnt, data + offset,
				    sizeof(struct p9_wstat),
				    &st);
		if (error < 0) {
			p9_debug(VFS, "returned %d\n", error);
			return -EIO;
		}

		memset(&dirent, 0, sizeof(struct dirent));
		// Convert the qid into ino and then put into dirent.
		//memcpy(&dirent.d_fileno, &st.qid, sizeof(st.>qid));
		if (dirent.d_fileno) {
			dirent.d_type = st.type;
			strncpy(dirent.d_name, st.name, strlen(st.name));
			dirent.d_reclen = GENERIC_DIRSIZ(&dirent);
		}

		/*
		 * If there isn't enough space in the uio to return a
		 * whole dirent, break off read
		 */
		if (uio->uio_resid < GENERIC_DIRSIZ(&dirent))
			break;

		/* Transfer */
		if (dirent.d_fileno)
			uiomove(&dirent, GENERIC_DIRSIZ(&dirent), uio);

		/* Advance */
		diroffset += dirent.d_reclen;
		offset += dirent.d_reclen;

		transoffset = diroffset;
	}

	/* Pass on last transferred offset */
	uio->uio_offset = transoffset;

	return (error);
}

static int
virtfs_readlink(struct vop_readlink_args *ap)
{
	return 0;
}

static int
virtfs_inactive(struct vop_inactive_args *ap)
{
	return (0);
}

struct vop_vector virtfs_vnops = {
	.vop_default =		&default_vnodeops,
	.vop_lookup =		vfs_cache_lookup,
	.vop_cachedlookup =	virtfs_lookup,
	.vop_open =		virtfs_open,
	.vop_close =		virtfs_close,
	.vop_getattr =		virtfs_getattr,
	.vop_setattr =		virtfs_setattr,
	.vop_readdir =		virtfs_readdir,
	.vop_create =		virtfs_create,
	.vop_mknod =		virtfs_mknod,
	.vop_read =		virtfs_read,
	.vop_write =		virtfs_write,
	.vop_fsync =		virtfs_fsync,
	.vop_remove =		virtfs_remove,
	.vop_link =		virtfs_link,
	.vop_rename =		virtfs_rename,
	.vop_mkdir =		virtfs_mkdir,
	.vop_rmdir =		virtfs_rmdir,
	.vop_symlink =		virtfs_symlink,
	.vop_readlink =		virtfs_readlink,
	.vop_inactive =		virtfs_inactive,
};
