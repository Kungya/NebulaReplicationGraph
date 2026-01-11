CL
- 1.0v : Skeleton Code of PrecomputedVisibilityGrid2D Node.
- 1.1v : WIP

https://technology.riotgames.com/news/demolishing-wallhacks-valorants-fog-war
<br/>
<br/>
![image](https://github.com/user-attachments/assets/384cbad8-1567-4f6f-83cf-5fbe846194a4)

<br/>

Simple Precomputed Visibility FogOfWar (Potentially Visible Set) using ReplicationGrpah.
- Inspired by Valorant and Dave Ratti's Approach.


Instead of naive collision detection using raycasts, we use the character's WorldLocation to determine the GridCell it belongs to. <br/>Each frame, we track the locations of Dynamic Actors to determine if the GridCell it belongs to has changed. We query the LookupTable corresponding to the GridCell it belongs to, returning a list of potentially visible GridCells. We replicate the Actors within that GridCell. <br/><br/>
The LookupTable only needs to be precomputed once. Various methods exist, including precomputing the specified pivot points in each cell by raycasting between them, and then manually correcting any missing GridCells.
If the number of pivot points is too large, precomputing them is computationally expensive. Therefore, we haven't found a fully automated precomputation method. There's a trade-off between the number of pivot points and the precomputation time.
<br/>
<br/>



$TODO LIST
- Generate detailed Visibility Info in PVSLookupTable (heuristic, terribly need a lot of work even though current GridCells' count is 7x7)
- Add Static/Dormancy Actor func
- Enable Pause Replication to reduce actor's respawn overhead (or, use NetDormancy)
- Process to block MulticastRPC when enemy actor is hiding
- Even if we can't see enemy actor, should still be able to hear its sound
- To reduce memory footprint, need to compress Cell Index's type size; FIntPoint into bit.
- porting to Iris's Dynamic Filter in the future
